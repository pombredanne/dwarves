/*
  Copyright (C) 2008 Arnaldo Carvalho de Melo <acme@redhat.com>

  This program is free software; you can redistribute it and/or modify it
  under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.
*/

#include <assert.h>
#include <dirent.h>
#include <dwarf.h>
#include <elfutils/libdwfl.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <libelf.h>
#include <obstack.h>
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "list.h"
#include "dwarves.h"
#include "dutil.h"
#include "strings.h"
#include "hash.h"

struct strings *strings;

#ifndef DW_AT_GNU_vector
#define DW_AT_GNU_vector 0x2107
#endif

#define hashtags__fn(key) hash_64(key, HASHTAGS__BITS)

static void __tag__print_not_supported(uint32_t tag, const char *func)
{
#ifdef STB_GNU_UNIQUE
	static bool dwarf_tags_warned[DW_TAG_rvalue_reference_type];
	static bool dwarf_gnu_tags_warned[DW_TAG_GNU_formal_parameter_pack - DW_TAG_MIPS_loop];
#else
	static bool dwarf_tags_warned[DW_TAG_shared_type];
	static bool dwarf_gnu_tags_warned[DW_TAG_class_template - DW_TAG_MIPS_loop];
#endif

	if (tag < DW_TAG_MIPS_loop) {
		if (dwarf_tags_warned[tag])
			return;
		dwarf_tags_warned[tag] = true;
	} else {
		uint32_t t = tag - DW_TAG_MIPS_loop;

		if (dwarf_gnu_tags_warned[t])
			return;
		dwarf_gnu_tags_warned[t] = true;
	}

	fprintf(stderr, "%s: tag not supported (%s)!\n", func,
		dwarf_tag_name(tag));
}

#define tag__print_not_supported(tag) \
	__tag__print_not_supported(tag, __func__)

struct dwarf_tag {
	struct hlist_node hash_node;
	Dwarf_Off	 type;
	Dwarf_Off	 id;
	union {
		Dwarf_Off abstract_origin;
		Dwarf_Off containing_type;
	};
	struct tag	 *tag;
	strings_t        decl_file;
	uint16_t         decl_line;
	uint16_t         small_id;
};

static Dwarf_Off dwarf_tag__spec(struct dwarf_tag *self)
{
	return *(Dwarf_Off *)(self + 1);
}

static void dwarf_tag__set_spec(struct dwarf_tag *self, Dwarf_Off spec)
{
	*(Dwarf_Off *)(self + 1) = spec;
}

#define HASHTAGS__BITS 8
#define HASHTAGS__SIZE (1UL << HASHTAGS__BITS)

#define obstack_chunk_alloc malloc
#define obstack_chunk_free free

static void *obstack_zalloc(struct obstack *self, size_t size)
{
	void *o = obstack_alloc(self, size);

	if (o)
		memset(o, 0, size);
	return o;
}

struct dwarf_cu {
	struct hlist_head hash_tags[HASHTAGS__SIZE];
	struct hlist_head hash_types[HASHTAGS__SIZE];
	struct obstack obstack;
	struct cu *cu;
};

static void dwarf_cu__init(struct dwarf_cu *self)
{
	unsigned int i;
	for (i = 0; i < HASHTAGS__SIZE; ++i) {
		INIT_HLIST_HEAD(&self->hash_tags[i]);
		INIT_HLIST_HEAD(&self->hash_types[i]);
	}
	obstack_init(&self->obstack);
}

static void hashtags__hash(struct hlist_head *hashtable,
			   struct dwarf_tag *dtag)
{
	struct hlist_head *head = hashtable + hashtags__fn(dtag->id);
	hlist_add_head(&dtag->hash_node, head);
}

static struct dwarf_tag *hashtags__find(const struct hlist_head *hashtable,
					const Dwarf_Off id)
{
	if (id == 0)
		return NULL;

	struct dwarf_tag *tpos;
	struct hlist_node *pos;
	uint16_t bucket = hashtags__fn(id);
	const struct hlist_head *head = hashtable + bucket;

	hlist_for_each_entry(tpos, pos, head, hash_node) {
		if (tpos->id == id)
			return tpos;
	}

	return NULL;
}

static void cu__hash(struct cu *self, struct tag *tag)
{
	struct dwarf_cu *dcu = self->priv;
	struct hlist_head *hashtable = tag__is_tag_type(tag) ?
							dcu->hash_types :
							dcu->hash_tags;
	hashtags__hash(hashtable, tag->priv);
}

static struct dwarf_tag *dwarf_cu__find_tag_by_id(const struct dwarf_cu *self,
						  const Dwarf_Off id)
{
	return self ? hashtags__find(self->hash_tags, id) : NULL;
}

static struct dwarf_tag *dwarf_cu__find_type_by_id(const struct dwarf_cu *self,
						   const Dwarf_Off id)
{
	return self ? hashtags__find(self->hash_types, id) : NULL;
}

extern struct strings *strings;

static void *memdup(const void *src, size_t len, struct cu *cu)
{
	void *s = obstack_alloc(&cu->obstack, len);
	if (s != NULL)
		memcpy(s, src, len);
	return s;
}

/* Number decoding macros.  See 7.6 Variable Length Data.  */

#define get_uleb128_step(var, addr, nth, break)			\
	__b = *(addr)++;					\
	var |= (uintmax_t) (__b & 0x7f) << (nth * 7);		\
	if ((__b & 0x80) == 0)					\
		break

#define get_uleb128_rest_return(var, i, addrp)			\
	do {							\
		for (; i < 10; ++i) {				\
			get_uleb128_step(var, *addrp, i,	\
					  return var);		\
	}							\
	/* Other implementations set VALUE to UINT_MAX in this	\
	  case. So we better do this as well.  */		\
	return UINT64_MAX;					\
  } while (0)

static uint64_t __libdw_get_uleb128(uint64_t acc, uint32_t i,
				    const uint8_t **addrp)
{
	uint8_t __b;
	get_uleb128_rest_return (acc, i, addrp);
}

#define get_uleb128(var, addr)					\
	do {							\
		uint8_t __b;				\
		var = 0;					\
		get_uleb128_step(var, addr, 0, break);		\
		var = __libdw_get_uleb128 (var, 1, &(addr));	\
	} while (0)

static uint64_t attr_numeric(Dwarf_Die *die, uint32_t name)
{
	Dwarf_Attribute attr;
	uint32_t form;

	if (dwarf_attr(die, name, &attr) == NULL)
		return 0;

	form = dwarf_whatform(&attr);

	switch (form) {
	case DW_FORM_addr: {
		Dwarf_Addr addr;
		if (dwarf_formaddr(&attr, &addr) == 0)
			return addr;
	}
		break;
	case DW_FORM_data1:
	case DW_FORM_data2:
	case DW_FORM_data4:
	case DW_FORM_data8:
	case DW_FORM_sdata:
	case DW_FORM_udata: {
		Dwarf_Word value;
		if (dwarf_formudata(&attr, &value) == 0)
			return value;
	}
		break;
	case DW_FORM_flag:
		return 1;
	default:
		fprintf(stderr, "DW_AT_<0x%x>=0x%x\n", name, form);
		break;
	}

	return 0;
}

static uint64_t dwarf_expr(const uint8_t *expr, uint32_t len __unused)
{
	/* Common case: offset from start of the class */
	if (expr[0] == DW_OP_plus_uconst ||
	    expr[0] == DW_OP_constu) {
		uint64_t result;
		++expr;
		get_uleb128(result, expr);
		return result;
	}

	fprintf(stderr, "%s: unhandled %#x DW_OP_ operation\n",
		__func__, *expr);
	return UINT64_MAX;
}

static Dwarf_Off attr_offset(Dwarf_Die *die, const uint32_t name)
{
	Dwarf_Attribute attr;
	Dwarf_Block block;

	if (dwarf_attr(die, name, &attr) == NULL)
		return 0;

	switch (dwarf_whatform(&attr)) {
	case DW_FORM_data1:
	case DW_FORM_data2:
	case DW_FORM_sdata:
	case DW_FORM_udata: {
		Dwarf_Word value;
		if (dwarf_formudata(&attr, &value) == 0)
			return value;
		break;
	}
	default:
		if (dwarf_formblock(&attr, &block) == 0)
			return dwarf_expr(block.data, block.length);
	}

	return 0;
}

static const char *attr_string(Dwarf_Die *die, uint32_t name)
{
	Dwarf_Attribute attr;
	if (dwarf_attr(die, name, &attr) != NULL)
		return dwarf_formstring(&attr);
	return NULL;
}

static Dwarf_Off attr_type(Dwarf_Die *die, uint32_t attr_name)
{
	Dwarf_Attribute attr;
	if (dwarf_attr(die, attr_name, &attr) != NULL) {
		 Dwarf_Die type_die;
		 if (dwarf_formref_die(&attr, &type_die) != NULL)
			return dwarf_dieoffset(&type_die);

	}
	return 0;
}

static int attr_location(Dwarf_Die *die, Dwarf_Op **expr, size_t *exprlen)
{
	Dwarf_Attribute attr;
	if (dwarf_attr(die, DW_AT_location, &attr) != NULL) {
		if (dwarf_getlocation(&attr, expr, exprlen) == 0)
			return 0;
	}

	return 1;
}

static void *__tag__alloc(struct dwarf_cu *dcu, size_t size, bool spec)
{
	struct dwarf_tag *dtag = obstack_alloc(&dcu->obstack,
					       (sizeof(*dtag) +
					       (spec ? sizeof(Dwarf_Off) : 0)));
	if (dtag == NULL)
		return NULL;

	struct tag *self = obstack_alloc(&dcu->cu->obstack, size);

	if (self == NULL)
		return NULL;

	dtag->tag = self;
	self->priv = dtag;
	dtag->type = 0;
	self->type = 0;
	self->top_level = 0;

	return self;
}

static void *tag__alloc(struct cu *cu, size_t size)
{
	return __tag__alloc(cu->priv, size, false);
}

static void *tag__alloc_with_spec(struct cu *cu, size_t size)
{
	return __tag__alloc(cu->priv, size, true);
}

static void tag__init(struct tag *self, struct cu *cu, Dwarf_Die *die)
{
	struct dwarf_tag *dtag = self->priv;

	self->tag = dwarf_tag(die);

	dtag->id  = dwarf_dieoffset(die);

	if (self->tag == DW_TAG_imported_module ||
	    self->tag == DW_TAG_imported_declaration)
		dtag->type = attr_type(die, DW_AT_import);
	else
		dtag->type = attr_type(die, DW_AT_type);

	dtag->abstract_origin = attr_type(die, DW_AT_abstract_origin);
	self->recursivity_level = 0;

	if (cu->extra_dbg_info) {
		int32_t decl_line;
		const char *decl_file = dwarf_decl_file(die);
		static const char *last_decl_file;
		static uint32_t last_decl_file_idx;

		if (decl_file != last_decl_file) {
			last_decl_file_idx = strings__add(strings, decl_file);
			last_decl_file = decl_file;
		}

		dtag->decl_file = last_decl_file_idx;
		dwarf_decl_line(die, &decl_line);
		dtag->decl_line = decl_line;
	}

	INIT_LIST_HEAD(&self->node);
}

static struct tag *tag__new(Dwarf_Die *die, struct cu *cu)
{
	struct tag *self = tag__alloc(cu, sizeof(*self));

	if (self != NULL)
		tag__init(self, cu, die);

	return self;
}

static struct ptr_to_member_type *ptr_to_member_type__new(Dwarf_Die *die,
							  struct cu *cu)
{
	struct ptr_to_member_type *self = tag__alloc(cu, sizeof(*self));

	if (self != NULL) {
		tag__init(&self->tag, cu, die);
		struct dwarf_tag *dself = self->tag.priv;
		dself->containing_type = attr_type(die, DW_AT_containing_type);
	}

	return self;
}

static struct base_type *base_type__new(Dwarf_Die *die, struct cu *cu)
{
	struct base_type *self = tag__alloc(cu, sizeof(*self));

	if (self != NULL) {
		tag__init(&self->tag, cu, die);
		self->name = strings__add(strings, attr_string(die, DW_AT_name));
		self->bit_size = attr_numeric(die, DW_AT_byte_size) * 8;
		uint64_t encoding = attr_numeric(die, DW_AT_encoding);
		self->is_bool = encoding == DW_ATE_boolean;
		self->is_signed = encoding == DW_ATE_signed;
		self->is_varargs = false;
		self->name_has_encoding = true;
	}

	return self;
}

static struct array_type *array_type__new(Dwarf_Die *die, struct cu *cu)
{
	struct array_type *self = tag__alloc(cu, sizeof(*self));

	if (self != NULL) {
		tag__init(&self->tag, cu, die);
		self->dimensions = 0;
		self->nr_entries = NULL;
		self->is_vector	 = dwarf_hasattr(die, DW_AT_GNU_vector);
	}

	return self;
}

static void namespace__init(struct namespace *self, Dwarf_Die *die,
			    struct cu *cu)
{
	tag__init(&self->tag, cu, die);
	INIT_LIST_HEAD(&self->tags);
	self->sname = 0;
	self->name    = strings__add(strings, attr_string(die, DW_AT_name));
	self->nr_tags = 0;
	self->shared_tags = 0;
}

static struct namespace *namespace__new(Dwarf_Die *die, struct cu *cu)
{
	struct namespace *self = tag__alloc(cu, sizeof(*self));

	if (self != NULL)
		namespace__init(self, die, cu);

	return self;
}

static void type__init(struct type *self, Dwarf_Die *die, struct cu *cu)
{
	namespace__init(&self->namespace, die, cu);
	INIT_LIST_HEAD(&self->node);
	self->size		 = attr_numeric(die, DW_AT_byte_size);
	self->declaration	 = attr_numeric(die, DW_AT_declaration);
	dwarf_tag__set_spec(self->namespace.tag.priv,
			    attr_type(die, DW_AT_specification));
	self->definition_emitted = 0;
	self->fwd_decl_emitted	 = 0;
	self->resized		 = 0;
	self->nr_members	 = 0;
}

static struct type *type__new(Dwarf_Die *die, struct cu *cu)
{
	struct type *self = tag__alloc_with_spec(cu, sizeof(*self));

	if (self != NULL)
		type__init(self, die, cu);

	return self;
}

static struct enumerator *enumerator__new(Dwarf_Die *die, struct cu *cu)
{
	struct enumerator *self = tag__alloc(cu, sizeof(*self));

	if (self != NULL) {
		tag__init(&self->tag, cu, die);
		self->name = strings__add(strings, attr_string(die, DW_AT_name));
		self->value = attr_numeric(die, DW_AT_const_value);
	}

	return self;
}

static enum vlocation dwarf__location(Dwarf_Die *die, uint64_t *addr)
{
	Dwarf_Op *expr;
	size_t exprlen;
	enum vlocation location = LOCATION_UNKNOWN;

	if (attr_location(die, &expr, &exprlen) != 0)
		location = LOCATION_OPTIMIZED;
	else if (exprlen != 0)
		switch (expr->atom) {
		case DW_OP_addr:
			location = LOCATION_GLOBAL;
			*addr = expr[0].number;
			break;
		case DW_OP_reg1 ... DW_OP_reg31:
		case DW_OP_breg0 ... DW_OP_breg31:
			location = LOCATION_REGISTER;	break;
		case DW_OP_fbreg:
			location = LOCATION_LOCAL;	break;
		}

	return location;
}

static struct variable *variable__new(Dwarf_Die *die, struct cu *cu)
{
	struct variable *self = tag__alloc(cu, sizeof(*self));

	if (self != NULL) {
		tag__init(&self->ip.tag, cu, die);
		self->name = strings__add(strings, attr_string(die, DW_AT_name));
		/* variable is visible outside of its enclosing cu */
		self->external = dwarf_hasattr(die, DW_AT_external);
		/* non-defining declaration of an object */
		self->declaration = dwarf_hasattr(die, DW_AT_declaration);
		self->location = LOCATION_UNKNOWN;
		self->ip.addr = 0;
		if (!self->declaration && cu->has_addr_info)
			self->location = dwarf__location(die, &self->ip.addr);
	}

	return self;
}

int tag__recode_dwarf_bitfield(struct tag *self, struct cu *cu, uint16_t bit_size)
{
	uint16_t id;
	struct tag *recoded;
	/* in all the cases the name is at the same offset */
	strings_t name = tag__namespace(self)->name;

	switch (self->tag) {
	case DW_TAG_typedef: {
		const struct dwarf_tag *dself = self->priv;
		struct dwarf_tag *dtype = dwarf_cu__find_type_by_id(cu->priv,
								    dself->type);
		struct tag *type = dtype->tag;

		id = tag__recode_dwarf_bitfield(type, cu, bit_size);
		if (id == self->type)
			return id;

		struct type *new_typedef = obstack_zalloc(&cu->obstack,
							  sizeof(*new_typedef));
		if (new_typedef == NULL)
			return -ENOMEM;

		recoded = (struct tag *)new_typedef;
		recoded->tag = DW_TAG_typedef;
		recoded->type = id;
		new_typedef->namespace.name = tag__namespace(self)->name;
	}
		break;

	case DW_TAG_const_type:
	case DW_TAG_volatile_type: {
		const struct dwarf_tag *dself = self->priv;
		struct dwarf_tag *dtype = dwarf_cu__find_type_by_id(cu->priv,
								    dself->type);
		struct tag *type = dtype->tag;

		id = tag__recode_dwarf_bitfield(type, cu, bit_size);
		if (id == self->type)
			return id;

		recoded = obstack_zalloc(&cu->obstack, sizeof(*recoded));
		if (recoded == NULL)
			return -ENOMEM;

		recoded->tag = DW_TAG_volatile_type;
		recoded->type = id;
	}
		break;

	case DW_TAG_base_type:
		/*
		 * Here we must search on the final, core cu, not on
		 * the dwarf_cu as in dwarf there are no such things
		 * as base_types of less than 8 bits, etc.
		 */
		recoded = cu__find_base_type_by_sname_and_size(cu, name, bit_size, &id);
		if (recoded != NULL)
			return id;


		struct base_type *new_bt = obstack_zalloc(&cu->obstack,
							  sizeof(*new_bt));
		if (new_bt == NULL)
			return -ENOMEM;

		recoded = (struct tag *)new_bt;
		recoded->tag = DW_TAG_base_type;
		recoded->top_level = 1;
		new_bt->name = name;
		new_bt->bit_size = bit_size;
		break;

	case DW_TAG_enumeration_type:
		/*
		 * Here we must search on the final, core cu, not on
		 * the dwarf_cu as in dwarf there are no such things
		 * as enumeration_types of less than 8 bits, etc.
		 */
		recoded = cu__find_enumeration_by_sname_and_size(cu, name,
								 bit_size, &id);
		if (recoded != NULL)
			return id;

		struct type *alias = tag__type(self);
		struct type *new_enum = obstack_zalloc(&cu->obstack, sizeof(*new_enum));
		if (new_enum == NULL)
			return -ENOMEM;

		recoded = (struct tag *)new_enum;
		recoded->tag = DW_TAG_enumeration_type;
		recoded->top_level = 1;
		new_enum->nr_members = alias->nr_members;
		/*
		 * Share the tags
		 */
		new_enum->namespace.tags.next = &alias->namespace.tags;
		new_enum->namespace.shared_tags = 1;
		new_enum->namespace.name = name;
		new_enum->size = bit_size;
		break;
	default:
		fprintf(stderr, "%s: tag=%s, name=%s, bit_size=%d\n",
			__func__, dwarf_tag_name(self->tag),
			strings__ptr(strings, name), bit_size);
		return -EINVAL;
	}

	long new_id = -1;
	if (cu__add_tag(cu, recoded, &new_id) == 0)
		return new_id;

	obstack_free(&cu->obstack, recoded);
	return -ENOMEM;
}

int class_member__dwarf_recode_bitfield(struct class_member *self,
					struct cu *cu)
{
	struct dwarf_tag *dtag = self->tag.priv;
	struct dwarf_tag *type = dwarf_cu__find_type_by_id(cu->priv, dtag->type);
	int recoded_type_id = tag__recode_dwarf_bitfield(type->tag, cu,
							 self->bitfield_size);
	if (recoded_type_id < 0)
		return recoded_type_id;

	self->tag.type = recoded_type_id;
	return 0;
}

static struct class_member *class_member__new(Dwarf_Die *die, struct cu *cu)
{
	struct class_member *self = tag__alloc(cu, sizeof(*self));

	if (self != NULL) {
		tag__init(&self->tag, cu, die);
		self->name = strings__add(strings, attr_string(die, DW_AT_name));
		self->byte_offset = attr_offset(die, DW_AT_data_member_location);
		/*
		 * Will be cached later, in class_member__cache_byte_size
		 */
		self->byte_size = 0;
		self->bitfield_offset = attr_numeric(die, DW_AT_bit_offset);
		self->bitfield_size = attr_numeric(die, DW_AT_bit_size);
		self->bit_offset = self->byte_offset * 8 + self->bitfield_offset;
		self->bit_hole = 0;
		self->bitfield_end = 0;
		self->visited = 0;
		self->accessibility = attr_numeric(die, DW_AT_accessibility);
		self->virtuality    = attr_numeric(die, DW_AT_virtuality);
		self->hole = 0;
	}

	return self;
}

static struct parameter *parameter__new(Dwarf_Die *die, struct cu *cu)
{
	struct parameter *self = tag__alloc(cu, sizeof(*self));

	if (self != NULL) {
		tag__init(&self->tag, cu, die);
		self->name = strings__add(strings, attr_string(die, DW_AT_name));
	}

	return self;
}

static struct inline_expansion *inline_expansion__new(Dwarf_Die *die,
						      struct cu *cu)
{
	struct inline_expansion *self = tag__alloc(cu, sizeof(*self));

	if (self != NULL) {
		struct dwarf_tag *dtag = self->ip.tag.priv;

		tag__init(&self->ip.tag, cu, die);
		dtag->decl_file =
			strings__add(strings, attr_string(die, DW_AT_call_file));
		dtag->decl_line = attr_numeric(die, DW_AT_call_line);
		dtag->type = attr_type(die, DW_AT_abstract_origin);
		self->ip.addr = 0;
		self->high_pc = 0;

		if (!cu->has_addr_info)
			goto out;

		if (dwarf_lowpc(die, &self->ip.addr))
			self->ip.addr = 0;
		if (dwarf_lowpc(die, &self->high_pc))
			self->high_pc = 0;

		self->size = self->high_pc - self->ip.addr;
		if (self->size == 0) {
			Dwarf_Addr base, start;
			ptrdiff_t offset = 0;

			while (1) {
				offset = dwarf_ranges(die, offset, &base, &start,
						      &self->high_pc);
				start = (unsigned long)start;
				self->high_pc = (unsigned long)self->high_pc;
				if (offset <= 0)
					break;
				self->size += self->high_pc - start;
				if (self->ip.addr == 0)
					self->ip.addr = start;
			}
		}
	}
out:
	return self;
}

static struct label *label__new(Dwarf_Die *die, struct cu *cu)
{
	struct label *self = tag__alloc(cu, sizeof(*self));

	if (self != NULL) {
		tag__init(&self->ip.tag, cu, die);
		self->name = strings__add(strings, attr_string(die, DW_AT_name));
		if (!cu->has_addr_info || dwarf_lowpc(die, &self->ip.addr))
			self->ip.addr = 0;
	}

	return self;
}

static struct class *class__new(Dwarf_Die *die, struct cu *cu)
{
	struct class *self = tag__alloc_with_spec(cu, sizeof(*self));

	if (self != NULL) {
		type__init(&self->type, die, cu);
		INIT_LIST_HEAD(&self->vtable);
		self->nr_vtable_entries =
		  self->nr_holes =
		  self->nr_bit_holes =
		  self->padding =
		  self->bit_padding = 0;
		self->priv = NULL;
	}

	return self;
}

static void lexblock__init(struct lexblock *self, struct cu *cu,
			   Dwarf_Die *die)
{
	Dwarf_Off high_pc;

	if (!cu->has_addr_info || dwarf_lowpc(die, &self->ip.addr)) {
		self->ip.addr = 0;
		self->size = 0;
	} else if (dwarf_highpc(die, &high_pc))
		self->size = 0;
	else
		self->size = high_pc - self->ip.addr;

	INIT_LIST_HEAD(&self->tags);

	self->size_inline_expansions =
	self->nr_inline_expansions =
		self->nr_labels =
		self->nr_lexblocks =
		self->nr_variables = 0;
}

static struct lexblock *lexblock__new(Dwarf_Die *die, struct cu *cu)
{
	struct lexblock *self = tag__alloc(cu, sizeof(*self));

	if (self != NULL) {
		tag__init(&self->ip.tag, cu, die);
		lexblock__init(self, cu, die);
	}

	return self;
}

static void ftype__init(struct ftype *self, Dwarf_Die *die, struct cu *cu)
{
	const uint16_t tag = dwarf_tag(die);
	assert(tag == DW_TAG_subprogram || tag == DW_TAG_subroutine_type);

	tag__init(&self->tag, cu, die);
	INIT_LIST_HEAD(&self->parms);
	self->nr_parms	   = 0;
	self->unspec_parms = 0;
}

static struct ftype *ftype__new(Dwarf_Die *die, struct cu *cu)
{
	struct ftype *self = tag__alloc(cu, sizeof(*self));

	if (self != NULL)
		ftype__init(self, die, cu);

	return self;
}

static struct function *function__new(Dwarf_Die *die, struct cu *cu)
{
	struct function *self = tag__alloc_with_spec(cu, sizeof(*self));

	if (self != NULL) {
		ftype__init(&self->proto, die, cu);
		lexblock__init(&self->lexblock, cu, die);
		self->name     = strings__add(strings, attr_string(die, DW_AT_name));
		self->linkage_name = strings__add(strings, attr_string(die, DW_AT_MIPS_linkage_name));
		self->inlined  = attr_numeric(die, DW_AT_inline);
		self->external = dwarf_hasattr(die, DW_AT_external);
		self->abstract_origin = dwarf_hasattr(die, DW_AT_abstract_origin);
		dwarf_tag__set_spec(self->proto.tag.priv,
				    attr_type(die, DW_AT_specification));
		self->accessibility   = attr_numeric(die, DW_AT_accessibility);
		self->virtuality      = attr_numeric(die, DW_AT_virtuality);
		INIT_LIST_HEAD(&self->vtable_node);
		INIT_LIST_HEAD(&self->tool_node);
		self->vtable_entry    = -1;
		if (dwarf_hasattr(die, DW_AT_vtable_elem_location))
			self->vtable_entry = attr_offset(die, DW_AT_vtable_elem_location);
		self->cu_total_size_inline_expansions = 0;
		self->cu_total_nr_inline_expansions = 0;
		self->priv = NULL;
	}

	return self;
}

static uint64_t attr_upper_bound(Dwarf_Die *die)
{
	Dwarf_Attribute attr;

	if (dwarf_attr(die, DW_AT_upper_bound, &attr) != NULL) {
		Dwarf_Word num;

		if (dwarf_formudata(&attr, &num) == 0) {
			return (uintmax_t)num + 1;
		}
	}

	return 0;
}

static void __cu__tag_not_handled(Dwarf_Die *die, const char *fn)
{
	uint32_t tag = dwarf_tag(die);

	fprintf(stderr, "%s: DW_TAG_%s (%#x) @ <%#llx> not handled!\n",
		fn, dwarf_tag_name(tag), tag,
		(unsigned long long)dwarf_dieoffset(die));
}

#define cu__tag_not_handled(die) __cu__tag_not_handled(die, __FUNCTION__)

static struct tag *__die__process_tag(Dwarf_Die *die, struct cu *cu,
				      int toplevel, const char *fn);

#define die__process_tag(die, cu, toplevel) \
	__die__process_tag(die, cu, toplevel, __FUNCTION__)

static struct tag *die__create_new_tag(Dwarf_Die *die, struct cu *cu)
{
	struct tag *self = tag__new(die, cu);

	if (self != NULL) {
		if (dwarf_haschildren(die))
			fprintf(stderr, "%s: %s WITH children!\n", __func__,
				dwarf_tag_name(self->tag));
	}

	return self;
}

static struct tag *die__create_new_ptr_to_member_type(Dwarf_Die *die,
						      struct cu *cu)
{
	struct ptr_to_member_type *self = ptr_to_member_type__new(die, cu);

	return self ? &self->tag : NULL;
}

static int die__process_class(Dwarf_Die *die,
			      struct type *class, struct cu *cu);

static struct tag *die__create_new_class(Dwarf_Die *die, struct cu *cu)
{
	Dwarf_Die child;
	struct class *class = class__new(die, cu);

	if (class != NULL &&
	    dwarf_haschildren(die) != 0 &&
	    dwarf_child(die, &child) == 0) {
		if (die__process_class(&child, &class->type, cu) != 0) {
			class__delete(class, cu);
			class = NULL;
		}
	}

	return class ? &class->type.namespace.tag : NULL;
}

static int die__process_namespace(Dwarf_Die *die, struct namespace *namespace,
				  struct cu *cu);

static struct tag *die__create_new_namespace(Dwarf_Die *die, struct cu *cu)
{
	Dwarf_Die child;
	struct namespace *namespace = namespace__new(die, cu);

	if (namespace != NULL &&
	    dwarf_haschildren(die) != 0 &&
	    dwarf_child(die, &child) == 0) {
		if (die__process_namespace(&child, namespace, cu) != 0) {
			namespace__delete(namespace, cu);
			namespace = NULL;
		}
	}

	return namespace ? &namespace->tag : NULL;
}

static struct tag *die__create_new_union(Dwarf_Die *die, struct cu *cu)
{
	Dwarf_Die child;
	struct type *utype = type__new(die, cu);

	if (utype != NULL &&
	    dwarf_haschildren(die) != 0 &&
	    dwarf_child(die, &child) == 0) {
		if (die__process_class(&child, utype, cu) != 0) {
			type__delete(utype, cu);
			utype = NULL;
		}
	}

	return utype ? &utype->namespace.tag : NULL;
}

static struct tag *die__create_new_base_type(Dwarf_Die *die, struct cu *cu)
{
	struct base_type *base = base_type__new(die, cu);

	if (base == NULL)
		return NULL;

	if (dwarf_haschildren(die))
		fprintf(stderr, "%s: DW_TAG_base_type WITH children!\n",
			__func__);

	return &base->tag;
}

static struct tag *die__create_new_typedef(Dwarf_Die *die, struct cu *cu)
{
	struct type *tdef = type__new(die, cu);

	if (tdef == NULL)
		return NULL;

	if (dwarf_haschildren(die)) {
		struct dwarf_tag *dtag = tdef->namespace.tag.priv;
		fprintf(stderr, "%s: DW_TAG_typedef %llx WITH children!\n",
			__func__, (unsigned long long)dtag->id);
	}

	return &tdef->namespace.tag;
}

static struct tag *die__create_new_array(Dwarf_Die *die, struct cu *cu)
{
	Dwarf_Die child;
	/* "64 dimensions will be enough for everybody." acme, 2006 */
	const uint8_t max_dimensions = 64;
	uint32_t nr_entries[max_dimensions];
	struct array_type *array = array_type__new(die, cu);

	if (array == NULL)
		return NULL;

	if (!dwarf_haschildren(die) || dwarf_child(die, &child) != 0)
		return &array->tag;

	die = &child;
	do {
		if (dwarf_tag(die) == DW_TAG_subrange_type) {
			nr_entries[array->dimensions++] = attr_upper_bound(die);
			if (array->dimensions == max_dimensions) {
				fprintf(stderr, "%s: only %u dimensions are "
						"supported!\n",
					__FUNCTION__, max_dimensions);
				break;
			}
		} else
			cu__tag_not_handled(die);
	} while (dwarf_siblingof(die, die) == 0);

	array->nr_entries = memdup(nr_entries,
				   array->dimensions * sizeof(uint32_t), cu);
	if (array->nr_entries == NULL)
		goto out_free;

	return &array->tag;
out_free:
	obstack_free(&cu->obstack, array);
	return NULL;
}

static struct tag *die__create_new_parameter(Dwarf_Die *die,
					     struct ftype *ftype,
					     struct lexblock *lexblock,
					     struct cu *cu)
{
	struct parameter *parm = parameter__new(die, cu);

	if (parm == NULL)
		return NULL;

	if (ftype != NULL)
		ftype__add_parameter(ftype, parm);
	else {
		/*
		 * DW_TAG_formal_parameters on a non DW_TAG_subprogram nor
		 * DW_TAG_subroutine_type tag happens sometimes, likely due to
		 * compiler optimizing away a inline expansion (at least this
		 * was observed in some cases, such as in the Linux kernel
		 * current_kernel_time function circa 2.6.20-rc5), keep it in
		 * the lexblock tag list because it can be referenced as an
		 * DW_AT_abstract_origin in another DW_TAG_formal_parameter.
		*/
		lexblock__add_tag(lexblock, &parm->tag);
	}

	return &parm->tag;
}

static struct tag *die__create_new_label(Dwarf_Die *die,
					 struct lexblock *lexblock,
					 struct cu *cu)
{
	struct label *label = label__new(die, cu);

	if (label == NULL)
		return NULL;

	lexblock__add_label(lexblock, label);
	return &label->ip.tag;
}

static struct tag *die__create_new_variable(Dwarf_Die *die, struct cu *cu)
{
	struct variable *var = variable__new(die, cu);

	return var ? &var->ip.tag : NULL;
}

static struct tag *die__create_new_subroutine_type(Dwarf_Die *die,
						   struct cu *cu)
{
	Dwarf_Die child;
	struct ftype *ftype = ftype__new(die, cu);
	struct tag *tag;

	if (ftype == NULL)
		return NULL;

	if (!dwarf_haschildren(die) || dwarf_child(die, &child) != 0)
		goto out;

	die = &child;
	do {
		long id = -1;

		switch (dwarf_tag(die)) {
		case DW_TAG_formal_parameter:
			tag = die__create_new_parameter(die, ftype, NULL, cu);
			break;
		case DW_TAG_unspecified_parameters:
			ftype->unspec_parms = 1;
			continue;
		default:
			tag = die__process_tag(die, cu, 0);
			if (tag == NULL)
				goto out_delete;

			if (cu__add_tag(cu, tag, &id) < 0)
				goto out_delete_tag;

			goto hash;
		}

		if (tag == NULL)
			goto out_delete;

		if (cu__table_add_tag(cu, tag, &id) < 0)
			goto out_delete_tag;
hash:
		cu__hash(cu, tag);
		struct dwarf_tag *dtag = tag->priv;
		dtag->small_id = id;
	} while (dwarf_siblingof(die, die) == 0);
out:
	return &ftype->tag;
out_delete_tag:
	tag__delete(tag, cu);
out_delete:
	ftype__delete(ftype, cu);
	return NULL;
}

static struct tag *die__create_new_enumeration(Dwarf_Die *die, struct cu *cu)
{
	Dwarf_Die child;
	struct type *enumeration = type__new(die, cu);

	if (enumeration == NULL)
		return NULL;

	if (enumeration->size == 0)
		enumeration->size = sizeof(int) * 8;
	else
		enumeration->size *= 8;

	if (!dwarf_haschildren(die) || dwarf_child(die, &child) != 0) {
		/* Seen on libQtCore.so.4.3.4.debug,
		 * class QAbstractFileEngineIterator, enum EntryInfoType */
		goto out;
	}

	die = &child;
	do {
		struct enumerator *enumerator;

		if (dwarf_tag(die) != DW_TAG_enumerator) {
			cu__tag_not_handled(die);
			continue;
		}
		enumerator = enumerator__new(die, cu);
		if (enumerator == NULL)
			goto out_delete;

		enumeration__add(enumeration, enumerator);
	} while (dwarf_siblingof(die, die) == 0);
out:
	return &enumeration->namespace.tag;
out_delete:
	enumeration__delete(enumeration, cu);
	return NULL;
}

static int die__process_class(Dwarf_Die *die, struct type *class,
			      struct cu *cu)
{
	do {
		switch (dwarf_tag(die)) {
#ifdef STB_GNU_UNIQUE
		case DW_TAG_GNU_template_template_param:
		case DW_TAG_GNU_template_parameter_pack:
#endif
		case DW_TAG_template_type_parameter:
		case DW_TAG_template_value_parameter:
			/* FIXME: probably we'll have to attach this as a list of
 			 * template parameters to use at class__fprintf time... */
			tag__print_not_supported(dwarf_tag(die));
			continue;
		case DW_TAG_inheritance:
		case DW_TAG_member: {
			struct class_member *member = class_member__new(die, cu);

			if (member == NULL)
				return -ENOMEM;

			if (cu__is_c_plus_plus(cu)) {
				long id = -1;

				if (cu__table_add_tag(cu, &member->tag, &id) < 0) {
					class_member__delete(member, cu);
					return -ENOMEM;
				}

				struct dwarf_tag *dtag = member->tag.priv;
				dtag->small_id = id;
			}

			type__add_member(class, member);
			cu__hash(cu, &member->tag);
		}
			continue;
		default: {
			struct tag *tag = die__process_tag(die, cu, 0);

			if (tag == NULL)
				return -ENOMEM;

			long id = -1;

			if (cu__table_add_tag(cu, tag, &id) < 0) {
				tag__delete(tag, cu);
				return -ENOMEM;
			}

			struct dwarf_tag *dtag = tag->priv;
			dtag->small_id = id;

			namespace__add_tag(&class->namespace, tag);
			cu__hash(cu, tag);
			if (tag__is_function(tag)) {
				struct function *fself = tag__function(tag);

				if (fself->vtable_entry != -1)
					class__add_vtable_entry(type__class(class), fself);
			}
			continue;
		}
		}
	} while (dwarf_siblingof(die, die) == 0);

	return 0;
}

static int die__process_namespace(Dwarf_Die *die, struct namespace *namespace,
				  struct cu *cu)
{
	struct tag *tag;
	do {
		tag = die__process_tag(die, cu, 0);
		if (tag == NULL)
			goto out_enomem;

		long id = -1;
		if (cu__table_add_tag(cu, tag, &id) < 0)
			goto out_delete_tag;

		struct dwarf_tag *dtag = tag->priv;
		dtag->small_id = id;

		namespace__add_tag(namespace, tag);
		cu__hash(cu, tag);
	} while (dwarf_siblingof(die, die) == 0);

	return 0;
out_delete_tag:
	tag__delete(tag, cu);
out_enomem:
	return -ENOMEM;
}

static int die__process_function(Dwarf_Die *die, struct ftype *ftype,
				  struct lexblock *lexblock, struct cu *cu);

static int die__create_new_lexblock(Dwarf_Die *die,
				    struct cu *cu, struct lexblock *father)
{
	struct lexblock *lexblock = lexblock__new(die, cu);

	if (lexblock != NULL) {
		if (die__process_function(die, NULL, lexblock, cu) != 0)
			goto out_delete;
	}
	if (father != NULL)
		lexblock__add_lexblock(father, lexblock);
	return 0;
out_delete:
	lexblock__delete(lexblock, cu);
	return -ENOMEM;
}

static struct tag *die__create_new_inline_expansion(Dwarf_Die *die,
						    struct lexblock *lexblock,
						    struct cu *cu);

static int die__process_inline_expansion(Dwarf_Die *die, struct cu *cu)
{
	Dwarf_Die child;
	struct tag *tag;

	if (!dwarf_haschildren(die) || dwarf_child(die, &child) != 0)
		return 0;

	die = &child;
	do {
		long id = -1;

		switch (dwarf_tag(die)) {
		case DW_TAG_lexical_block:
			if (die__create_new_lexblock(die, cu, NULL) != 0)
				goto out_enomem;
			continue;
		case DW_TAG_formal_parameter:
			/*
			 * FIXME:
			 * So far DW_TAG_inline_routine had just an
			 * abstract origin, but starting with
			 * /usr/lib/openoffice.org/basis3.0/program/libdbalx.so
			 * I realized it really has to be handled as a
			 * DW_TAG_function... Lets just get the types
			 * for 1.8, then fix this properly.
			 *
			 * cu__tag_not_handled(die);
			 */
			continue;
		case DW_TAG_inlined_subroutine:
			tag = die__create_new_inline_expansion(die, NULL, cu);
			break;
		default:
			tag = die__process_tag(die, cu, 0);
			if (tag == NULL)
				goto out_enomem;

			if (cu__add_tag(cu, tag, &id) < 0)
				goto out_delete_tag;
			goto hash;
		}

		if (tag == NULL)
			goto out_enomem;

		if (cu__table_add_tag(cu, tag, &id) < 0)
			goto out_delete_tag;
hash:
		cu__hash(cu, tag);
		struct dwarf_tag *dtag = tag->priv;
		dtag->small_id = id;
	} while (dwarf_siblingof(die, die) == 0);

	return 0;
out_delete_tag:
	tag__delete(tag, cu);
out_enomem:
	return -ENOMEM;
}

static struct tag *die__create_new_inline_expansion(Dwarf_Die *die,
						    struct lexblock *lexblock,
						    struct cu *cu)
{
	struct inline_expansion *exp = inline_expansion__new(die, cu);

	if (exp == NULL)
		return NULL;

	if (die__process_inline_expansion(die, cu) != 0) {
		obstack_free(&cu->obstack, exp);
		return NULL;
	}

	if (lexblock != NULL)
		lexblock__add_inline_expansion(lexblock, exp);
	return &exp->ip.tag;
}

static int die__process_function(Dwarf_Die *die, struct ftype *ftype,
				 struct lexblock *lexblock, struct cu *cu)
{
	Dwarf_Die child;
	struct tag *tag;

	if (!dwarf_haschildren(die) || dwarf_child(die, &child) != 0)
		return 0;

	die = &child;
	do {
		long id = -1;

		switch (dwarf_tag(die)) {
		case DW_TAG_template_type_parameter:
		case DW_TAG_template_value_parameter:
			/* FIXME: probably we'll have to attach this as a list of
 			 * template parameters to use at class__fprintf time... 
 			 * See die__process_class */
			tag__print_not_supported(dwarf_tag(die));
			continue;
		case DW_TAG_formal_parameter:
			tag = die__create_new_parameter(die, ftype, lexblock, cu);
			break;
		case DW_TAG_variable:
			tag = die__create_new_variable(die, cu);
			if (tag == NULL)
				goto out_enomem;
			lexblock__add_variable(lexblock, tag__variable(tag));
			break;
		case DW_TAG_unspecified_parameters:
			if (ftype != NULL)
				ftype->unspec_parms = 1;
			continue;
		case DW_TAG_label:
			tag = die__create_new_label(die, lexblock, cu);
			break;
		case DW_TAG_inlined_subroutine:
			tag = die__create_new_inline_expansion(die, lexblock, cu);
			break;
		case DW_TAG_lexical_block:
			if (die__create_new_lexblock(die, cu, lexblock) != 0)
				goto out_enomem;
			continue;
		default:
			tag = die__process_tag(die, cu, 0);
			if (tag == NULL)
				goto out_enomem;

			if (cu__add_tag(cu, tag, &id) < 0)
				goto out_delete_tag;

			goto hash;
		}

		if (tag == NULL)
			goto out_enomem;

		if (cu__table_add_tag(cu, tag, &id) < 0)
			goto out_delete_tag;
hash:
		cu__hash(cu, tag);
		struct dwarf_tag *dtag = tag->priv;
		dtag->small_id = id;
	} while (dwarf_siblingof(die, die) == 0);

	return 0;
out_delete_tag:
	tag__delete(tag, cu);
out_enomem:
	return -ENOMEM;
}

static struct tag *die__create_new_function(Dwarf_Die *die, struct cu *cu)
{
	struct function *function = function__new(die, cu);

	if (function != NULL &&
	    die__process_function(die, &function->proto,
				  &function->lexblock, cu) != 0) {
		function__delete(function, cu);
		function = NULL;
	}

	return function ? &function->proto.tag : NULL;
}

static struct tag *__die__process_tag(Dwarf_Die *die, struct cu *cu,
				      int top_level, const char *fn)
{
	struct tag *tag;

	switch (dwarf_tag(die)) {
	case DW_TAG_array_type:
		tag = die__create_new_array(die, cu);		break;
	case DW_TAG_base_type:
		tag = die__create_new_base_type(die, cu);	break;
	case DW_TAG_const_type:
	case DW_TAG_imported_declaration:
	case DW_TAG_imported_module:
	case DW_TAG_pointer_type:
	case DW_TAG_reference_type:
	case DW_TAG_volatile_type:
		tag = die__create_new_tag(die, cu);		break;
	case DW_TAG_ptr_to_member_type:
		tag = die__create_new_ptr_to_member_type(die, cu); break;
	case DW_TAG_enumeration_type:
		tag = die__create_new_enumeration(die, cu);	break;
	case DW_TAG_namespace:
		tag = die__create_new_namespace(die, cu);	break;
	case DW_TAG_class_type:
	case DW_TAG_interface_type:
	case DW_TAG_structure_type:
		tag = die__create_new_class(die, cu);		break;
	case DW_TAG_subprogram:
		tag = die__create_new_function(die, cu);	break;
	case DW_TAG_subroutine_type:
		tag = die__create_new_subroutine_type(die, cu);	break;
	case DW_TAG_typedef:
		tag = die__create_new_typedef(die, cu);		break;
	case DW_TAG_union_type:
		tag = die__create_new_union(die, cu);		break;
	case DW_TAG_variable:
		tag = die__create_new_variable(die, cu);	break;
	default:
		__cu__tag_not_handled(die, fn);
		tag = NULL;
		break;
	}

	if (tag != NULL)
		tag->top_level = top_level;

	return tag;
}

static int die__process_unit(Dwarf_Die *die, struct cu *cu)
{
	do {
		struct tag *tag = die__process_tag(die, cu, 1);
		if (tag == NULL)
			return -ENOMEM;

		long id = -1;
		cu__add_tag(cu, tag, &id);
		cu__hash(cu, tag);
		struct dwarf_tag *dtag = tag->priv;
		dtag->small_id = id;
	} while (dwarf_siblingof(die, die) == 0);

	return 0;
}

static void __tag__print_type_not_found(struct tag *self, const char *func)
{
	struct dwarf_tag *dtag = self->priv;
	fprintf(stderr, "%s: couldn't find %#llx type for %#llx (%s)!\n", func,
		(unsigned long long)dtag->type, (unsigned long long)dtag->id,
		dwarf_tag_name(self->tag));
}

#define tag__print_type_not_found(self) \
	__tag__print_type_not_found(self, __func__)

static void ftype__recode_dwarf_types(struct tag *self, struct cu *cu);

static int namespace__recode_dwarf_types(struct tag *self, struct cu *cu)
{
	struct tag *pos;
	struct dwarf_cu *dcu = cu->priv;
	struct namespace *ns = tag__namespace(self);

	namespace__for_each_tag(ns, pos) {
		struct dwarf_tag *dtype;
		struct dwarf_tag *dpos = pos->priv;

		if (tag__has_namespace(pos)) {
			if (namespace__recode_dwarf_types(pos, cu))
				return -1;
			continue;
		}

		switch (pos->tag) {
		case DW_TAG_member: {
			struct class_member *member = tag__class_member(pos);
			/*
			 * We may need to recode the type, possibly creating a
			 * suitably sized new base_type
			 */
			if (member->bitfield_size != 0) {
				if (class_member__dwarf_recode_bitfield(member, cu))
					return -1;
				continue;
			}
		}
			break;
		case DW_TAG_subroutine_type:
		case DW_TAG_subprogram:
			ftype__recode_dwarf_types(pos, cu);
			break;
		case DW_TAG_imported_module:
			dtype = dwarf_cu__find_tag_by_id(dcu, dpos->type);
			goto check_type;
		/* Can be for both types and non types */
		case DW_TAG_imported_declaration:
			dtype = dwarf_cu__find_tag_by_id(dcu, dpos->type);
			if (dtype != NULL)
				goto next;
			goto find_type;
		}

		if (dpos->type == 0) /* void */
			continue;
find_type:
		dtype = dwarf_cu__find_type_by_id(dcu, dpos->type);
check_type:
		if (dtype == NULL) {
			tag__print_type_not_found(pos);
			continue;
		}
next:
		pos->type = dtype->small_id;
	}
	return 0;
}

static void type__recode_dwarf_specification(struct tag *self, struct cu *cu)
{
	struct dwarf_tag *dtype;
	struct type *t = tag__type(self);
	Dwarf_Off specification = dwarf_tag__spec(self->priv);

	if (t->namespace.name != 0 || specification == 0)
		return;

	dtype = dwarf_cu__find_type_by_id(cu->priv, specification);
	if (dtype != NULL)
		t->namespace.name = tag__namespace(dtype->tag)->name;
	else {
		struct dwarf_tag *dtag = self->priv;

		fprintf(stderr,
			"%s: couldn't find name for "
			"class %#llx, specification=%#llx\n", __func__,
			(unsigned long long)dtag->id,
			(unsigned long long)specification);
	}
}

static void __tag__print_abstract_origin_not_found(struct tag *self,
						   const char *func)
{
	struct dwarf_tag *dtag = self->priv;
	fprintf(stderr,
		"%s: couldn't find %#llx abstract_origin for %#llx (%s)!\n",
		func, (unsigned long long)dtag->abstract_origin,
		(unsigned long long)dtag->id,
		dwarf_tag_name(self->tag));
}

#define tag__print_abstract_origin_not_found(self ) \
	__tag__print_abstract_origin_not_found(self, __func__)

static void ftype__recode_dwarf_types(struct tag *self, struct cu *cu)
{
	struct parameter *pos;
	struct dwarf_cu *dcu = cu->priv;
	struct ftype *type = tag__ftype(self);

	ftype__for_each_parameter(type, pos) {
		struct dwarf_tag *dpos = pos->tag.priv;
		struct dwarf_tag *dtype;

		if (dpos->type == 0) {
			if (dpos->abstract_origin == 0) {
				/* Function without parameters */
				pos->tag.type = 0;
				continue;
			}
			dtype = dwarf_cu__find_tag_by_id(dcu, dpos->abstract_origin);
			if (dtype == NULL) {
				tag__print_abstract_origin_not_found(&pos->tag);
				continue;
			}
			pos->name = tag__parameter(dtype->tag)->name;
			pos->tag.type = dtype->tag->type;
			continue;
		}

		dtype = dwarf_cu__find_type_by_id(dcu, dpos->type);
		if (dtype == NULL) {
			tag__print_type_not_found(&pos->tag);
			continue;
		}
		pos->tag.type = dtype->small_id;
	}
}

static void lexblock__recode_dwarf_types(struct lexblock *self, struct cu *cu)
{
	struct tag *pos;
	struct dwarf_cu *dcu = cu->priv;

	list_for_each_entry(pos, &self->tags, node) {
		struct dwarf_tag *dpos = pos->priv;
		struct dwarf_tag *dtype;

		switch (pos->tag) {
		case DW_TAG_lexical_block:
			lexblock__recode_dwarf_types(tag__lexblock(pos), cu);
			continue;
		case DW_TAG_inlined_subroutine:
			dtype = dwarf_cu__find_tag_by_id(dcu, dpos->type);
			if (dtype == NULL) {
				tag__print_type_not_found(pos);
				continue;
			}
			ftype__recode_dwarf_types(dtype->tag, cu);
			continue;

		case DW_TAG_formal_parameter:
			if (dpos->type != 0)
				break;

			struct parameter *fp = tag__parameter(pos);
			dtype = dwarf_cu__find_tag_by_id(dcu,
							 dpos->abstract_origin);
			if (dtype == NULL) {
				tag__print_abstract_origin_not_found(pos);
				continue;
			}
			fp->name = tag__parameter(dtype->tag)->name;
			pos->type = dtype->tag->type;
			continue;

		case DW_TAG_variable:
			if (dpos->type != 0)
				break;

			struct variable *var = tag__variable(pos);

			if (dpos->abstract_origin == 0) {
				/*
				 * DW_TAG_variable completely empty was
				 * found on libQtGui.so.4.3.4.debug
				 * <3><d6ea1>: Abbrev Number: 164 (DW_TAG_variable)
				 */
				continue;
			}

			dtype = dwarf_cu__find_tag_by_id(dcu,
							 dpos->abstract_origin);
			if (dtype == NULL) {
				tag__print_abstract_origin_not_found(pos);
				continue;
			}
			var->name = tag__variable(dtype->tag)->name;
			pos->type = dtype->tag->type;
			continue;

		case DW_TAG_label: {
			struct label *l = tag__label(pos);

			if (dpos->abstract_origin == 0)
				continue;

			dtype = dwarf_cu__find_tag_by_id(dcu, dpos->abstract_origin);
			if (dtype != NULL)
				l->name = tag__label(dtype->tag)->name;
			else
				tag__print_abstract_origin_not_found(pos);
		}
			continue;
		}

		dtype = dwarf_cu__find_type_by_id(dcu, dpos->type);
		if (dtype == NULL) {
			tag__print_type_not_found(pos);
			continue;
		}
		pos->type = dtype->small_id;
	}
}

static int tag__recode_dwarf_type(struct tag *self, struct cu *cu)
{
	struct dwarf_tag *dtag = self->priv;
	struct dwarf_tag *dtype;

	/* Check if this is an already recoded bitfield */
	if (dtag == NULL)
		return 0;

	if (tag__is_type(self))
		type__recode_dwarf_specification(self, cu);

	if (tag__has_namespace(self))
		return namespace__recode_dwarf_types(self, cu);

	switch (self->tag) {
	case DW_TAG_subprogram: {
		struct function *fn = tag__function(self);

		if (fn->name == 0)  {
			Dwarf_Off specification = dwarf_tag__spec(dtag);
			if (dtag->abstract_origin == 0 &&
			    specification == 0) {
				/*
				 * Found on libQtGui.so.4.3.4.debug
				 *  <3><1423de>: Abbrev Number: 209 (DW_TAG_subprogram)
				 *      <1423e0>   DW_AT_declaration : 1
				 */
				return 0;
			}
			dtype = dwarf_cu__find_tag_by_id(cu->priv, dtag->abstract_origin);
			if (dtype == NULL)
				dtype = dwarf_cu__find_tag_by_id(cu->priv, specification);
			if (dtype != NULL)
				fn->name = tag__function(dtype->tag)->name;
			else {
				fprintf(stderr,
					"%s: couldn't find name for "
					"function %#llx, abstract_origin=%#llx,"
					" specification=%#llx\n", __func__,
					(unsigned long long)dtag->id,
					(unsigned long long)dtag->abstract_origin,
					(unsigned long long)specification);
			}
		}
		lexblock__recode_dwarf_types(&fn->lexblock, cu);
	}
		/* Fall thru */

	case DW_TAG_subroutine_type:
		ftype__recode_dwarf_types(self, cu);
		/* Fall thru, for the function return type */
		break;

	case DW_TAG_lexical_block:
		lexblock__recode_dwarf_types(tag__lexblock(self), cu);
		return 0;

	case DW_TAG_ptr_to_member_type: {
		struct ptr_to_member_type *pt = tag__ptr_to_member_type(self);

		dtype = dwarf_cu__find_type_by_id(cu->priv, dtag->containing_type);
		if (dtype != NULL)
			pt->containing_type = dtype->small_id;
		else {
			fprintf(stderr,
				"%s: couldn't find type for "
				"containing_type %#llx, containing_type=%#llx\n",
				__func__,
				(unsigned long long)dtag->id,
				(unsigned long long)dtag->containing_type);
		}
	}
		break;

	case DW_TAG_namespace:
		return namespace__recode_dwarf_types(self, cu);
	/* Damn, DW_TAG_inlined_subroutine is an special case
           as dwarf_tag->id is in fact an abtract origin, i.e. must be
	   looked up in the tags_table, not in the types_table.
	   The others also point to routines, so are in tags_table */
	case DW_TAG_inlined_subroutine:
	case DW_TAG_imported_module:
		dtype = dwarf_cu__find_tag_by_id(cu->priv, dtag->type);
		goto check_type;
	/* Can be for both types and non types */
	case DW_TAG_imported_declaration:
		dtype = dwarf_cu__find_tag_by_id(cu->priv, dtag->type);
		if (dtype != NULL)
			goto out;
		goto find_type;
	}

	if (dtag->type == 0) {
		self->type = 0; /* void */
		return 0;
	}

find_type:
	dtype = dwarf_cu__find_type_by_id(cu->priv, dtag->type);
check_type:
	if (dtype == NULL) {
		tag__print_type_not_found(self);
		return 0;
	}
out:
	self->type = dtype->small_id;
	return 0;
}

static int cu__recode_dwarf_types_table(struct cu *self,
					struct ptr_table *pt,
					uint32_t i)
{
	for (; i < pt->nr_entries; ++i) {
		struct tag *tag = pt->entries[i];

		if (tag != NULL) /* void, see cu__new */
			if (tag__recode_dwarf_type(tag, self))
				return -1;
	}
	return 0;
}

static int cu__recode_dwarf_types(struct cu *self)
{
	if (cu__recode_dwarf_types_table(self, &self->types_table, 1) ||
	    cu__recode_dwarf_types_table(self, &self->tags_table, 0) ||
	    cu__recode_dwarf_types_table(self, &self->functions_table, 0))
		return -1;
	return 0;
}

static const char *dwarf_tag__decl_file(const struct tag *self,
					const struct cu *cu)
{
	struct dwarf_tag *dtag = self->priv;
	return cu->extra_dbg_info ?
			strings__ptr(strings, dtag->decl_file) : NULL;
}

static uint32_t dwarf_tag__decl_line(const struct tag *self,
				     const struct cu *cu)
{
	struct dwarf_tag *dtag = self->priv;
	return cu->extra_dbg_info ? dtag->decl_line : 0;
}

static unsigned long long dwarf_tag__orig_id(const struct tag *self,
					       const struct cu *cu)
{
	struct dwarf_tag *dtag = self->priv;
	return cu->extra_dbg_info ? dtag->id : 0;
}

static unsigned long long dwarf_tag__orig_type(const struct tag *self,
					       const struct cu *cu)
{
	struct dwarf_tag *dtag = self->priv;
	return cu->extra_dbg_info ? dtag->type : 0;
}

static const char *dwarf__strings_ptr(const struct cu *cu __unused,
				      strings_t s)
{
	return strings__ptr(strings, s);
}

struct debug_fmt_ops dwarf__ops;

static int die__process(Dwarf_Die *die, struct cu *cu)
{
	Dwarf_Die child;
	const uint16_t tag = dwarf_tag(die);

	if (tag != DW_TAG_compile_unit) {
		fprintf(stderr, "%s: DW_TAG_compile_unit expected got %s!\n",
			__FUNCTION__, dwarf_tag_name(tag));
		return -EINVAL;
	}

	cu->language = attr_numeric(die, DW_AT_language);

	if (dwarf_child(die, &child) == 0) {
		int err = die__process_unit(&child, cu);
		if (err)
			return err;
	}

	if (dwarf_siblingof(die, die) == 0)
		fprintf(stderr, "%s: got %s unexpected tag after "
				"DW_TAG_compile_unit!\n",
			__FUNCTION__, dwarf_tag_name(tag));

	return cu__recode_dwarf_types(cu);
}

static int class_member__cache_byte_size(struct tag *self, struct cu *cu,
					 void *cookie)
{
	if (self->tag == DW_TAG_member || self->tag == DW_TAG_inheritance) {
		struct conf_load *conf_load = cookie;
		struct class_member *member = tag__class_member(self);

		if (member->bitfield_size != 0) {
			struct tag *type = tag__follow_typedef(&member->tag, cu);
check_volatile:
			if (tag__is_volatile(type) || tag__is_const(type)) {
				type = tag__follow_typedef(type, cu);
				goto check_volatile;
			}

			uint16_t type_bit_size;
			size_t integral_bit_size;

			if (tag__is_enumeration(type)) {
				type_bit_size = tag__type(type)->size;
				integral_bit_size = sizeof(int) * 8; /* FIXME: always this size? */
			} else {
				struct base_type *bt = tag__base_type(type);
				type_bit_size = bt->bit_size;
				integral_bit_size = base_type__name_to_size(bt, cu);
			}
			/*
			 * XXX: integral_bit_size can be zero if base_type__name_to_size doesn't
			 * know about the base_type name, so one has to add there when
			 * such base_type isn't found. pahole will put zero on the
			 * struct output so it should be easy to spot the name when
			 * such unlikely thing happens.
			 */
			member->byte_size = integral_bit_size / 8;

			if (integral_bit_size == 0)
				return 0;

			if (type_bit_size == integral_bit_size) {
				member->bit_size = integral_bit_size;
				if (conf_load && conf_load->fixup_silly_bitfields) {
					member->bitfield_size = 0;
					member->bitfield_offset = 0;
				}
				return 0;
			}

			member->bit_size = type_bit_size;
		} else {
			member->byte_size = tag__size(self, cu);
			member->bit_size = member->byte_size * 8;
		}
	}

	return 0;
}

static int cus__load_module(struct cus *self, struct conf_load *conf,
			    Dwfl_Module *mod, Dwarf *dw, Elf *elf,
			    const char *filename)
{
	Dwarf_Off off = 0, noff;
	size_t cuhl;
	GElf_Addr vaddr;
	const unsigned char *build_id = NULL;

#ifdef HAVE_DWFL_MODULE_BUILD_ID
	int build_id_len = dwfl_module_build_id(mod, &build_id, &vaddr);
#else
	int build_id_len = 0;
#endif
	while (dwarf_nextcu(dw, off, &noff, &cuhl, NULL, NULL, NULL) == 0) {
		Dwarf_Die die_mem, tmp;
		Dwarf_Die *cu_die = dwarf_offdie(dw, off + cuhl, &die_mem);
		uint8_t pointer_size, offset_size;

		dwarf_diecu(cu_die, &tmp, &pointer_size, &offset_size);
		/*
		 * DW_AT_name in DW_TAG_compile_unit can be NULL, first
		 * seen in:
		 * /usr/libexec/gcc/x86_64-redhat-linux/4.3.2/ecj1.debug
		 */
		const char *name = attr_string(cu_die, DW_AT_name);
		struct cu *cu = cu__new(name ?: "", pointer_size,
					build_id, build_id_len, filename);
		if (cu == NULL)
			return DWARF_CB_ABORT;
		cu->uses_global_strings = true;
		cu->elf = elf;
		cu->dwfl = mod;
		cu->extra_dbg_info = conf ? conf->extra_dbg_info : 0;
		cu->has_addr_info = conf ? conf->get_addr_info : 0;

		struct dwarf_cu dcu;

		dwarf_cu__init(&dcu);
		dcu.cu = cu;
		cu->priv = &dcu;
		cu->dfops = &dwarf__ops;

		if (die__process(cu_die, cu) != 0)
			return DWARF_CB_ABORT;
		base_type_name_to_size_table__init(strings);
		cu__for_all_tags(cu, class_member__cache_byte_size, conf);
		off = noff;
		if (conf && conf->steal) {
			switch (conf->steal(cu, conf)) {
			case LSK__STOP_LOADING:
				return DWARF_CB_ABORT;
			case LSK__STOLEN:
				/*
				 * The app stole this cu, possibly deleting it,
				 * so forget about it:
				 */
				continue;
			case LSK__KEEPIT:
				break;
			}
		}

		if (!cu->extra_dbg_info)
			obstack_free(&dcu.obstack, NULL);

		cus__add(self, cu);
	}

	return DWARF_CB_OK;
}

struct process_dwflmod_parms {
	struct cus	 *cus;
	struct conf_load *conf;
	const char	 *filename;
	uint32_t	 nr_dwarf_sections_found;
};

static int cus__process_dwflmod(Dwfl_Module *dwflmod,
				void **userdata __unused,
				const char *name __unused,
				Dwarf_Addr base __unused,
				void *arg)
{
	struct process_dwflmod_parms *parms = arg;
	struct cus *self = parms->cus;

	GElf_Addr dwflbias;
	/*
	 * Does the relocation and saves the elf for later processing
	 * by the stealer, such as pahole_stealer, so that it don't
	 * have to create another Elf instance just to do things like
	 * reading this ELF file symtab to do CTF encoding of the
	 * DW_TAG_suprogram tags (functions).
	 */
	Elf *elf = dwfl_module_getelf(dwflmod, &dwflbias);

	Dwarf_Addr dwbias;
	Dwarf *dw = dwfl_module_getdwarf(dwflmod, &dwbias);

	int err = DWARF_CB_OK;
	if (dw != NULL) {
		++parms->nr_dwarf_sections_found;
		err = cus__load_module(self, parms->conf, dwflmod, dw, elf,
				       parms->filename);
	}
	/*
	 * XXX We will fall back to try finding other debugging
	 * formats (CTF), so no point in telling this to the user
	 * Use for debugging.
	 * else
	 *   fprintf(stderr,
	 *         "%s: can't get debug context descriptor: %s\n",
	 *	__func__, dwfl_errmsg(-1));
	 */

	return err;
}

static int cus__process_file(struct cus *self, struct conf_load *conf, int fd,
			     const char *filename)
{
	/* Duplicate an fd for dwfl_report_offline to swallow.  */
	int dwfl_fd = dup(fd);

	if (dwfl_fd < 0)
		return -1;

	/*
	 * Use libdwfl in a trivial way to open the libdw handle for us.
	 * This takes care of applying relocations to DWARF data in ET_REL
	 * files.
	 */

	static const Dwfl_Callbacks callbacks = {
		.section_address = dwfl_offline_section_address,
		.find_debuginfo	 = dwfl_standard_find_debuginfo,
		/* We use this table for core files too.  */
		.find_elf	 = dwfl_build_id_find_elf,
	};

	Dwfl *dwfl = dwfl_begin(&callbacks);

	if (dwfl_report_offline(dwfl, filename, filename, dwfl_fd) == NULL)
		return -1;

	dwfl_report_end(dwfl, NULL, NULL);

	struct process_dwflmod_parms parms = {
		.cus  = self,
		.conf = conf,
		.filename = filename,
		.nr_dwarf_sections_found = 0,
	};

	/* Process the one or more modules gleaned from this file. */
	dwfl_getmodules(dwfl, cus__process_dwflmod, &parms, 0);
	dwfl_end(dwfl);
	return parms.nr_dwarf_sections_found ? 0 : -1;
}

static int dwarf__load_file(struct cus *self, struct conf_load *conf,
			    const char *filename)
{
	int fd, err;

	elf_version(EV_CURRENT);

	fd = open(filename, O_RDONLY);

	if (fd == -1)
		return -1;

	err = cus__process_file(self, conf, fd, filename);
	close(fd);

	return err;
}

static int dwarf__init(void)
{
	strings = strings__new();
	return strings != NULL ? 0 : -ENOMEM;
}

static void dwarf__exit(void)
{
	strings__delete(strings);
	strings = NULL;
}

struct debug_fmt_ops dwarf__ops = {
	.name		     = "dwarf",
	.init		     = dwarf__init,
	.exit		     = dwarf__exit,
	.load_file	     = dwarf__load_file,
	.strings__ptr	     = dwarf__strings_ptr,
	.tag__decl_file	     = dwarf_tag__decl_file,
	.tag__decl_line	     = dwarf_tag__decl_line,
	.tag__orig_id	     = dwarf_tag__orig_id,
	.tag__orig_type	     = dwarf_tag__orig_type,
};
