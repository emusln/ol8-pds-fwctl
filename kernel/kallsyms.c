// SPDX-License-Identifier: GPL-2.0-only
/*
 * kallsyms.c: in-kernel printing of symbolic oopses and stack traces.
 *
 * Rewritten and vastly simplified by Rusty Russell for in-kernel
 * module loader:
 *   Copyright 2002 Rusty Russell <rusty@rustcorp.com.au> IBM Corporation
 *
 * ChangeLog:
 *
 * (25/Aug/2004) Paulo Marques <pmarques@grupopie.com>
 *      Changed the compression method from stem compression to "table lookup"
 *      compression (see scripts/kallsyms.c for a more complete description)
 */
#include <linux/kallsyms.h>
#include <linux/init.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/kdb.h>
#include <linux/err.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>	/* for cond_resched */
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/filter.h>
#include <linux/ftrace.h>
#include <linux/kprobes.h>
#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/module.h>
#include <linux/kernel.h>

#include "kallsyms_internal.h"

/*
 * Expand a compressed symbol data into the resulting uncompressed string,
 * if uncompressed string is too long (>= maxlen), it will be truncated,
 * given the offset to where the symbol is in the compressed stream.
 */
static unsigned int kallsyms_expand_symbol(unsigned int off,
					   char *result, size_t maxlen)
{
	int len, skipped_first = 0;
	const char *tptr;
	const u8 *data;

	/* Get the compressed symbol length from the first symbol byte. */
	data = &kallsyms_names[off];
	len = *data;
	data++;

	/*
	 * Update the offset to return the offset for the next symbol on
	 * the compressed stream.
	 */
	off += len + 1;

	/*
	 * For every byte on the compressed symbol data, copy the table
	 * entry for that byte.
	 */
	while (len) {
		tptr = &kallsyms_token_table[kallsyms_token_index[*data]];
		data++;
		len--;

		while (*tptr) {
			if (skipped_first) {
				if (maxlen <= 1)
					goto tail;
				*result = *tptr;
				result++;
				maxlen--;
			} else
				skipped_first = 1;
			tptr++;
		}
	}

tail:
	if (maxlen)
		*result = '\0';

	/* Return to offset to the next symbol. */
	return off;
}

/*
 * Get symbol type information. This is encoded as a single char at the
 * beginning of the symbol name.
 */
static char kallsyms_get_symbol_type(unsigned int off)
{
	/*
	 * Get just the first code, look it up in the token table,
	 * and return the first char from this token.
	 */
	return kallsyms_token_table[kallsyms_token_index[kallsyms_names[off + 1]]];
}


/*
 * Find the offset on the compressed stream given and index in the
 * kallsyms array.
 */
static unsigned int get_symbol_offset(unsigned long pos)
{
	const u8 *name;
	int i;

	/*
	 * Use the closest marker we have. We have markers every 256 positions,
	 * so that should be close enough.
	 */
	name = &kallsyms_names[kallsyms_markers[pos >> 8]];

	/*
	 * Sequentially scan all the symbols up to the point we're searching
	 * for. Every symbol is stored in a [<len>][<len> bytes of data] format,
	 * so we just need to add the len to the current pointer for every
	 * symbol we wish to skip.
	 */
	for (i = 0; i < (pos & 0xFF); i++)
		name = name + (*name) + 1;

	return name - kallsyms_names;
}

static unsigned long kallsyms_sym_address(int idx)
{
	if (!IS_ENABLED(CONFIG_KALLSYMS_BASE_RELATIVE))
		return kallsyms_addresses[idx];

	/* values are unsigned offsets if --absolute-percpu is not in effect */
	if (!IS_ENABLED(CONFIG_KALLSYMS_ABSOLUTE_PERCPU))
		return kallsyms_relative_base + (u32)kallsyms_offsets[idx];

	/* ...otherwise, positive offsets are absolute values */
	if (kallsyms_offsets[idx] >= 0)
		return kallsyms_offsets[idx];

	/* ...and negative offsets are relative to kallsyms_relative_base - 1 */
	return kallsyms_relative_base - 1 - kallsyms_offsets[idx];
}

#if defined(CONFIG_CFI_CLANG) && defined(CONFIG_LTO_CLANG_THIN)
/*
 * LLVM appends a hash to static function names when ThinLTO and CFI are
 * both enabled, i.e. foo() becomes foo$707af9a22804d33c81801f27dcfe489b.
 * This causes confusion and potentially breaks user space tools, so we
 * strip the suffix from expanded symbol names.
 */
static inline bool cleanup_symbol_name(char *s)
{
	char *res;

	res = strrchr(s, '$');
	if (res)
		*res = '\0';

	return res != NULL;
}
#else
static inline bool cleanup_symbol_name(char *s) { return false; }
#endif

#ifdef CONFIG_KALLMODSYMS
static unsigned long kallsyms_builtin_module_address(int idx)
{
	if (!IS_ENABLED(CONFIG_KALLSYMS_BASE_RELATIVE))
		return kallsyms_module_addresses[idx];

	/* values are unsigned offsets if --absolute-percpu is not in effect */
	if (!IS_ENABLED(CONFIG_KALLSYMS_ABSOLUTE_PERCPU))
		return kallsyms_relative_base + (u32)kallsyms_module_offsets[idx];

	/* ...otherwise, positive offsets are absolute values */
	if (kallsyms_module_offsets[idx] >= 0)
		return kallsyms_module_offsets[idx];

	/* ...and negative offsets are relative to kallsyms_relative_base - 1 */
	return kallsyms_relative_base - 1 - kallsyms_module_offsets[idx];
}
#endif

static int compare_symbol_name(const char *name, char *namebuf)
{
	int ret;

	ret = strcmp(name, namebuf);
	if (!ret)
		return ret;

	if (cleanup_symbol_name(namebuf) && !strcmp(name, namebuf))
		return 0;

	return ret;
}

static unsigned int get_symbol_seq(int index)
{
	unsigned int i, seq = 0;

	for (i = 0; i < 3; i++)
		seq = (seq << 8) | kallsyms_seqs_of_names[3 * index + i];

	return seq;
}

static int kallsyms_lookup_names(const char *name,
				 unsigned int *start,
				 unsigned int *end)
{
	int ret;
	int low, mid, high;
	unsigned int seq, off;
	char namebuf[KSYM_NAME_LEN];

	low = 0;
	high = kallsyms_num_syms - 1;

	while (low <= high) {
		mid = low + (high - low) / 2;
		seq = get_symbol_seq(mid);
		off = get_symbol_offset(seq);
		kallsyms_expand_symbol(off, namebuf, ARRAY_SIZE(namebuf));
		ret = compare_symbol_name(name, namebuf);
		if (ret > 0)
			low = mid + 1;
		else if (ret < 0)
			high = mid - 1;
		else
			break;
	}

	if (low > high)
		return -ESRCH;

	low = mid;
	while (low) {
		seq = get_symbol_seq(low - 1);
		off = get_symbol_offset(seq);
		kallsyms_expand_symbol(off, namebuf, ARRAY_SIZE(namebuf));
		if (compare_symbol_name(name, namebuf))
			break;
		low--;
	}
	*start = low;

	if (end) {
		high = mid;
		while (high < kallsyms_num_syms - 1) {
			seq = get_symbol_seq(high + 1);
			off = get_symbol_offset(seq);
			kallsyms_expand_symbol(off, namebuf, ARRAY_SIZE(namebuf));
			if (compare_symbol_name(name, namebuf))
				break;
			high++;
		}
		*end = high;
	}

	return 0;
}

/* Lookup the address for this symbol. Returns 0 if not found. */
unsigned long kallsyms_lookup_name(const char *name)
{
	int ret;
	unsigned int i;

	/* Skip the search for empty string. */
	if (!*name)
		return 0;

	ret = kallsyms_lookup_names(name, &i, NULL);
	if (!ret)
		return kallsyms_sym_address(get_symbol_seq(i));

	return module_kallsyms_lookup_name(name);
}

/*
 * Iterate over all symbols in vmlinux.  For symbols from modules use
 * module_kallsyms_on_each_symbol instead.
 */
int kallsyms_on_each_symbol(int (*fn)(void *, const char *, struct module *,
				      unsigned long),
			    void *data)
{
	char namebuf[KSYM_NAME_LEN];
	unsigned long i;
	unsigned int off;
	int ret;

	for (i = 0, off = 0; i < kallsyms_num_syms; i++) {
		off = kallsyms_expand_symbol(off, namebuf, ARRAY_SIZE(namebuf));
		ret = fn(data, namebuf, NULL, kallsyms_sym_address(i));
		if (ret != 0)
			return ret;
	}
	return 0;
}

/*
 * The caller passes in an address, and we return an index to the symbol --
 * potentially also size and offset information.
 * But an address might map to multiple symbols because:
 *   - some symbols might have zero size
 *   - some symbols might be aliases of one another
 *   - some symbols might span (encompass) others
 * The symbols should already be ordered so that, for a particular address,
 * we first have the zero-size ones, then the biggest, then the smallest.
 * So we find the index by:
 *   - finding the last symbol with the target address
 *   - backing the index up so long as both the address and size are unchanged
 */
static unsigned long get_symbol_pos(unsigned long addr,
				    unsigned long *symbolsize,
				    unsigned long *offset)
{
	unsigned long low, high, mid;

	/* This kernel should never had been booted. */
	if (!IS_ENABLED(CONFIG_KALLSYMS_BASE_RELATIVE))
		BUG_ON(!kallsyms_addresses);
	else
		BUG_ON(!kallsyms_offsets);

	/* Do a binary search on the sorted kallsyms_addresses array. */
	low = 0;
	high = kallsyms_num_syms;

	while (high - low > 1) {
		mid = low + (high - low) / 2;
		if (kallsyms_sym_address(mid) <= addr)
			low = mid;
		else
			high = mid;
	}

	/*
	 * Search for the first aliased symbol.
	 */
	while (low
	    && kallsyms_sym_address(low-1) == kallsyms_sym_address(low)
	    && kallsyms_sizes[low-1] == kallsyms_sizes[low])
		--low;

	if (symbolsize)
		*symbolsize = kallsyms_sizes[low];
	if (offset)
		*offset = addr - kallsyms_sym_address(low);

	return low;
}

/*
 * The caller passes in an address, and we return an index to the corresponding
 * builtin module index in .kallsyms_modules, or (unsigned long) -1 if none
 * match.
 *
 * The hint_idx, if set, is a hint as to the possible return value, to handle
 * the common case in which consecutive runs of addresses relate to the same
 * index.
 */
#ifdef CONFIG_KALLMODSYMS
static unsigned long get_builtin_module_idx(unsigned long addr, unsigned long hint_idx)
{
	unsigned long low, high, mid;

	if (!IS_ENABLED(CONFIG_KALLSYMS_BASE_RELATIVE))
		BUG_ON(!kallsyms_module_addresses);
	else
		BUG_ON(!kallsyms_module_offsets);

	/*
	 * Do a binary search on the sorted kallsyms_modules array.  The last
	 * entry in this array indicates the end of the text section, not an
	 * object file.
	 */
	low = 0;
	high = kallsyms_num_modules - 1;

	if (hint_idx > low && hint_idx < (high - 1) &&
	    addr >= kallsyms_builtin_module_address(hint_idx) &&
	    addr < kallsyms_builtin_module_address(hint_idx + 1))
		return hint_idx;

	if (addr >= kallsyms_builtin_module_address(low)
	    && addr < kallsyms_builtin_module_address(high)) {
		while (high - low > 1) {
			mid = low + (high - low) / 2;
			if (kallsyms_builtin_module_address(mid) <= addr)
				low = mid;
			else
				high = mid;
		}
		return low;
	}

	return (unsigned long) -1;
}
#endif

/*
 * Lookup an address but don't bother to find any names.
 */
int kallsyms_lookup_size_offset(unsigned long addr, unsigned long *symbolsize,
				unsigned long *offset)
{
	char namebuf[KSYM_NAME_LEN];

	if (is_ksym_addr(addr)) {
		get_symbol_pos(addr, symbolsize, offset);
		return 1;
	}
	return !!module_address_lookup(addr, symbolsize, offset, NULL, NULL, namebuf) ||
	       !!__bpf_address_lookup(addr, symbolsize, offset, namebuf);
}

static const char *kallsyms_lookup_buildid(unsigned long addr,
			unsigned long *symbolsize,
			unsigned long *offset, char **modname,
			const unsigned char **modbuildid, char *namebuf)
{
	const char *ret;

	namebuf[KSYM_NAME_LEN - 1] = 0;
	namebuf[0] = 0;

	if (is_ksym_addr(addr)) {
		unsigned long pos;

		pos = get_symbol_pos(addr, symbolsize, offset);
		/* Grab name */
		kallsyms_expand_symbol(get_symbol_offset(pos),
				       namebuf, KSYM_NAME_LEN);
		if (modname)
			*modname = NULL;
		if (modbuildid)
			*modbuildid = NULL;

		ret = namebuf;
		goto found;
	}

	/* See if it's in a module or a BPF JITed image. */
	ret = module_address_lookup(addr, symbolsize, offset,
				    modname, modbuildid, namebuf);
	if (!ret)
		ret = bpf_address_lookup(addr, symbolsize,
					 offset, modname, namebuf);

	if (!ret)
		ret = ftrace_mod_address_lookup(addr, symbolsize,
						offset, modname, namebuf);

found:
	cleanup_symbol_name(namebuf);
	return ret;
}

/*
 * Lookup an address
 * - modname is set to NULL if it's in the kernel.
 * - We guarantee that the returned name is valid until we reschedule even if.
 *   It resides in a module.
 * - We also guarantee that modname will be valid until rescheduled.
 */
const char *kallsyms_lookup(unsigned long addr,
			    unsigned long *symbolsize,
			    unsigned long *offset,
			    char **modname, char *namebuf)
{
	return kallsyms_lookup_buildid(addr, symbolsize, offset, modname,
				       NULL, namebuf);
}

int lookup_symbol_name(unsigned long addr, char *symname)
{
	int res;

	symname[0] = '\0';
	symname[KSYM_NAME_LEN - 1] = '\0';

	if (is_ksym_addr(addr)) {
		unsigned long pos;

		pos = get_symbol_pos(addr, NULL, NULL);
		/* Grab name */
		kallsyms_expand_symbol(get_symbol_offset(pos),
				       symname, KSYM_NAME_LEN);
		goto found;
	}
	/* See if it's in a module. */
	res = lookup_module_symbol_name(addr, symname);
	if (res)
		return res;

found:
	cleanup_symbol_name(symname);
	return 0;
}

int lookup_symbol_attrs(unsigned long addr, unsigned long *size,
			unsigned long *offset, char *modname, char *name)
{
	int res;

	name[0] = '\0';
	name[KSYM_NAME_LEN - 1] = '\0';

	if (is_ksym_addr(addr)) {
		unsigned long pos;

		pos = get_symbol_pos(addr, size, offset);
		/* Grab name */
		kallsyms_expand_symbol(get_symbol_offset(pos),
				       name, KSYM_NAME_LEN);
		modname[0] = '\0';
		goto found;
	}
	/* See if it's in a module. */
	res = lookup_module_symbol_attrs(addr, size, offset, modname, name);
	if (res)
		return res;

found:
	cleanup_symbol_name(name);
	return 0;
}

/* Look up a kernel symbol and return it in a text buffer. */
static int __sprint_symbol(char *buffer, unsigned long address,
			   int symbol_offset, int add_offset, int add_buildid)
{
	char *modname;
	const unsigned char *buildid;
	const char *name;
	unsigned long offset, size;
	int len;

	address += symbol_offset;
	name = kallsyms_lookup_buildid(address, &size, &offset, &modname, &buildid,
				       buffer);
	if (!name)
		return sprintf(buffer, "0x%lx", address - symbol_offset);

	if (name != buffer)
		strcpy(buffer, name);
	len = strlen(buffer);
	offset -= symbol_offset;

	if (add_offset)
		len += sprintf(buffer + len, "+%#lx/%#lx", offset, size);

	if (modname) {
		len += sprintf(buffer + len, " [%s", modname);
#if IS_ENABLED(CONFIG_STACKTRACE_BUILD_ID)
		if (add_buildid && buildid) {
			/* build ID should match length of sprintf */
#if IS_ENABLED(CONFIG_MODULES)
			static_assert(sizeof(typeof_member(struct module, build_id)) == 20);
#endif
			len += sprintf(buffer + len, " %20phN", buildid);
		}
#endif
		len += sprintf(buffer + len, "]");
	}

	return len;
}

/**
 * sprint_symbol - Look up a kernel symbol and return it in a text buffer
 * @buffer: buffer to be stored
 * @address: address to lookup
 *
 * This function looks up a kernel symbol with @address and stores its name,
 * offset, size and module name to @buffer if possible. If no symbol was found,
 * just saves its @address as is.
 *
 * This function returns the number of bytes stored in @buffer.
 */
int sprint_symbol(char *buffer, unsigned long address)
{
	return __sprint_symbol(buffer, address, 0, 1, 0);
}
EXPORT_SYMBOL_GPL(sprint_symbol);

/**
 * sprint_symbol_build_id - Look up a kernel symbol and return it in a text buffer
 * @buffer: buffer to be stored
 * @address: address to lookup
 *
 * This function looks up a kernel symbol with @address and stores its name,
 * offset, size, module name and module build ID to @buffer if possible. If no
 * symbol was found, just saves its @address as is.
 *
 * This function returns the number of bytes stored in @buffer.
 */
int sprint_symbol_build_id(char *buffer, unsigned long address)
{
	return __sprint_symbol(buffer, address, 0, 1, 1);
}
EXPORT_SYMBOL_GPL(sprint_symbol_build_id);

/**
 * sprint_symbol_no_offset - Look up a kernel symbol and return it in a text buffer
 * @buffer: buffer to be stored
 * @address: address to lookup
 *
 * This function looks up a kernel symbol with @address and stores its name
 * and module name to @buffer if possible. If no symbol was found, just saves
 * its @address as is.
 *
 * This function returns the number of bytes stored in @buffer.
 */
int sprint_symbol_no_offset(char *buffer, unsigned long address)
{
	return __sprint_symbol(buffer, address, 0, 0, 0);
}
EXPORT_SYMBOL_GPL(sprint_symbol_no_offset);

/**
 * sprint_backtrace - Look up a backtrace symbol and return it in a text buffer
 * @buffer: buffer to be stored
 * @address: address to lookup
 *
 * This function is for stack backtrace and does the same thing as
 * sprint_symbol() but with modified/decreased @address. If there is a
 * tail-call to the function marked "noreturn", gcc optimized out code after
 * the call so that the stack-saved return address could point outside of the
 * caller. This function ensures that kallsyms will find the original caller
 * by decreasing @address.
 *
 * This function returns the number of bytes stored in @buffer.
 */
int sprint_backtrace(char *buffer, unsigned long address)
{
	return __sprint_symbol(buffer, address, -1, 1, 0);
}

/**
 * sprint_backtrace_build_id - Look up a backtrace symbol and return it in a text buffer
 * @buffer: buffer to be stored
 * @address: address to lookup
 *
 * This function is for stack backtrace and does the same thing as
 * sprint_symbol() but with modified/decreased @address. If there is a
 * tail-call to the function marked "noreturn", gcc optimized out code after
 * the call so that the stack-saved return address could point outside of the
 * caller. This function ensures that kallsyms will find the original caller
 * by decreasing @address. This function also appends the module build ID to
 * the @buffer if @address is within a kernel module.
 *
 * This function returns the number of bytes stored in @buffer.
 */
int sprint_backtrace_build_id(char *buffer, unsigned long address)
{
	return __sprint_symbol(buffer, address, -1, 1, 1);
}

/* To avoid using get_symbol_offset for every symbol, we carry prefix along. */
struct kallsym_iter {
	loff_t pos;
	loff_t pos_arch_end;
	loff_t pos_mod_end;
	loff_t pos_ftrace_mod_end;
	loff_t pos_bpf_end;
	unsigned long value;
	unsigned int nameoff; /* If iterating in core kernel symbols. */
	unsigned long size;
	char type;
	char name[KSYM_NAME_LEN];
	char module_name[MODULE_NAME_LEN];
	const char *builtin_module_names;
	unsigned long hint_builtin_module_idx;
	int exported;
	int show_value;
};

int __weak arch_get_kallsym(unsigned int symnum, unsigned long *value,
			    char *type, char *name)
{
	return -EINVAL;
}

static int get_ksymbol_arch(struct kallsym_iter *iter)
{
	int ret = arch_get_kallsym(iter->pos - kallsyms_num_syms,
				   &iter->value, &iter->type,
				   iter->name);

	if (ret < 0) {
		iter->pos_arch_end = iter->pos;
		return 0;
	}

	return 1;
}

static int get_ksymbol_mod(struct kallsym_iter *iter)
{
	int ret = module_get_kallsym(iter->pos - iter->pos_arch_end,
				     &iter->value, &iter->type,
				     iter->name, iter->module_name,
				     &iter->size, &iter->exported);
	iter->builtin_module_names = NULL;

	if (ret < 0) {
		iter->pos_mod_end = iter->pos;
		return 0;
	}

	return 1;
}

/*
 * ftrace_mod_get_kallsym() may also get symbols for pages allocated for ftrace
 * purposes. In that case "__builtin__ftrace" is used as a module name, even
 * though "__builtin__ftrace" is not a module.
 */
static int get_ksymbol_ftrace_mod(struct kallsym_iter *iter)
{
	int ret = ftrace_mod_get_kallsym(iter->pos - iter->pos_mod_end,
					 &iter->value, &iter->type,
					 iter->name, iter->module_name,
					 &iter->exported);
	iter->builtin_module_names = NULL;

	if (ret < 0) {
		iter->pos_ftrace_mod_end = iter->pos;
		return 0;
	}

	return 1;
}

static int get_ksymbol_bpf(struct kallsym_iter *iter)
{
	int ret;

	strlcpy(iter->module_name, "bpf", MODULE_NAME_LEN);
	iter->exported = 0;
	iter->builtin_module_names = NULL;
	ret = bpf_get_kallsym(iter->pos - iter->pos_ftrace_mod_end,
			      &iter->value, &iter->type,
			      iter->name);
	if (ret < 0) {
		iter->pos_bpf_end = iter->pos;
		return 0;
	}

	return 1;
}

/*
 * This uses "__builtin__kprobes" as a module name for symbols for pages
 * allocated for kprobes' purposes, even though "__builtin__kprobes" is not a
 * module.
 */
static int get_ksymbol_kprobe(struct kallsym_iter *iter)
{
	strlcpy(iter->module_name, "__builtin__kprobes", MODULE_NAME_LEN);
	iter->exported = 0;
	iter->builtin_module_names = NULL;
	return kprobe_get_kallsym(iter->pos - iter->pos_bpf_end,
				  &iter->value, &iter->type,
				  iter->name) < 0 ? 0 : 1;
}

/* Returns space to next name. */
static unsigned long get_ksymbol_core(struct kallsym_iter *iter, int kallmodsyms)
{
	unsigned off = iter->nameoff;

	iter->exported = 0;
	iter->value = kallsyms_sym_address(iter->pos);

	iter->size = kallsyms_sizes[iter->pos];
	iter->type = kallsyms_get_symbol_type(off);

	iter->module_name[0] = '\0';
	iter->builtin_module_names = NULL;

	off = kallsyms_expand_symbol(off, iter->name, ARRAY_SIZE(iter->name));
#ifdef CONFIG_KALLMODSYMS
	if (kallmodsyms) {
		unsigned long mod_idx = (unsigned long) -1;

		if (kallsyms_module_offsets)
			mod_idx =
			  get_builtin_module_idx(iter->value,
						 iter->hint_builtin_module_idx);

		/*
		 * This is a built-in module iff the tables of built-in modules
		 * (address->module name mappings) and module names are known,
		 * and if the address was found there, and if the corresponding
		 * module index is nonzero.  All other cases mean off the end of
		 * the binary or in a non-modular range in between one or more
		 * modules.  (Also guard against a corrupt kallsyms_objfiles
		 * array pointing off the end of kallsyms_modules.)
		 */
		if (kallsyms_modules != NULL && kallsyms_module_names != NULL &&
		    mod_idx != (unsigned long) -1 &&
		    kallsyms_modules[mod_idx] != 0 &&
		    kallsyms_modules[mod_idx] < kallsyms_module_names_len)
			iter->builtin_module_names =
			  &kallsyms_module_names[kallsyms_modules[mod_idx]];
		iter->hint_builtin_module_idx = mod_idx;
	}
#endif
	return off - iter->nameoff;
}

static void reset_iter(struct kallsym_iter *iter, loff_t new_pos)
{
	iter->name[0] = '\0';
	iter->nameoff = get_symbol_offset(new_pos);
	iter->pos = new_pos;
	if (new_pos == 0) {
		iter->pos_arch_end = 0;
		iter->pos_mod_end = 0;
		iter->pos_ftrace_mod_end = 0;
		iter->pos_bpf_end = 0;
	}
}

/*
 * The end position (last + 1) of each additional kallsyms section is recorded
 * in iter->pos_..._end as each section is added, and so can be used to
 * determine which get_ksymbol_...() function to call next.
 */
static int update_iter_mod(struct kallsym_iter *iter, loff_t pos)
{
	iter->pos = pos;

	if ((!iter->pos_arch_end || iter->pos_arch_end > pos) &&
	    get_ksymbol_arch(iter))
		return 1;

	if ((!iter->pos_mod_end || iter->pos_mod_end > pos) &&
	    get_ksymbol_mod(iter))
		return 1;

	if ((!iter->pos_ftrace_mod_end || iter->pos_ftrace_mod_end > pos) &&
	    get_ksymbol_ftrace_mod(iter))
		return 1;

	if ((!iter->pos_bpf_end || iter->pos_bpf_end > pos) &&
	    get_ksymbol_bpf(iter))
		return 1;

	return get_ksymbol_kprobe(iter);
}

/* Returns false if pos at or past end of file. */
static int update_iter(struct kallsym_iter *iter, loff_t pos, int kallmodsyms)
{
	/* Module symbols can be accessed randomly. */
	if (pos >= kallsyms_num_syms)
		return update_iter_mod(iter, pos);

	/* If we're not on the desired position, reset to new position. */
	if (pos != iter->pos)
		reset_iter(iter, pos);

	iter->nameoff += get_ksymbol_core(iter, kallmodsyms);
	iter->pos++;

	return 1;
}

static void *s_next(struct seq_file *m, void *p, loff_t *pos)
{
	(*pos)++;

	if (!update_iter(m->private, *pos, 0))
		return NULL;
	return p;
}

static void *s_start(struct seq_file *m, loff_t *pos)
{
	if (!update_iter(m->private, *pos, 0))
		return NULL;
	return m->private;
}

static void s_stop(struct seq_file *m, void *p)
{
}

static int s_show_internal(struct seq_file *m, void *p, int kallmodsyms)
{
	void *value;
	struct kallsym_iter *iter = m->private;
	unsigned long size;

	/* Some debugging symbols have no name.  Ignore them. */
	if (!iter->name[0])
		return 0;

	value = iter->show_value ? (void *)iter->value : NULL;
	size = iter->show_value ? iter->size : 0;

	/*
	 * Real module, or built-in module and /proc/kallsyms being shown.
	 */
	if (iter->module_name[0] != '\0' ||
	    (iter->builtin_module_names != NULL && kallmodsyms != 0)) {
		char type;

		/*
		 * Label it "global" if it is exported, "local" if not exported.
		 */
		type = iter->exported ? toupper(iter->type) :
					tolower(iter->type);
#ifdef CONFIG_KALLMODSYMS
		if (kallmodsyms) {
			/*
			 * /proc/kallmodsyms, built as a module.
			 */
			if (iter->builtin_module_names == NULL)
				seq_printf(m, "%px %lx %c %s\t[%s]\n", value,
					   size, type, iter->name,
					   iter->module_name);
			/*
			 * /proc/kallmodsyms, single-module symbol.
			 */
			else if (*iter->builtin_module_names != '\0')
				seq_printf(m, "%px %lx %c %s\t[%s]\n", value,
					   size, type, iter->name,
					   iter->builtin_module_names);
			/*
			 * /proc/kallmodsyms, multimodule symbol.  Formatted
			 * as \0MODULE_COUNTmodule-1\0module-2\0, where
			 * MODULE_COUNT is a single byte, 2 or higher.
			 */
			else {
				size_t i = *(char *)(iter->builtin_module_names + 1);
				const char *walk = iter->builtin_module_names + 2;

				seq_printf(m, "%px %lx %c %s\t[%s]", value,
					   size, type, iter->name, walk);

                                while (--i > 0) {
					walk += strlen(walk) + 1;
					seq_printf (m, " [%s]", walk);
				}
				seq_printf(m, "\n");
			}
		} else				/* !kallmodsyms */
#endif /* CONFIG_KALLMODSYMS */
			seq_printf(m, "%px %c %s\t[%s]\n", value,
				   type, iter->name, iter->module_name);
	/*
	 * Non-modular, /proc/kallmodsyms -> print size.
	 */
	} else if (kallmodsyms)
		seq_printf(m, "%px %lx %c %s\n", value, size,
			   iter->type, iter->name);
	else
		seq_printf(m, "%px %c %s\n", value,
			   iter->type, iter->name);
	return 0;
}

static int s_show(struct seq_file *m, void *p)
{
	return s_show_internal(m, p, 0);
}

static const struct seq_operations kallsyms_op = {
	.start = s_start,
	.next = s_next,
	.stop = s_stop,
	.show = s_show
};

#ifdef CONFIG_KALLMODSYMS
static int s_mod_show(struct seq_file *m, void *p)
{
	return s_show_internal(m, p, 1);
}
static void *s_mod_next(struct seq_file *m, void *p, loff_t *pos)
{
	(*pos)++;

	if (!update_iter(m->private, *pos, 1))
		return NULL;
	return p;
}

static void *s_mod_start(struct seq_file *m, loff_t *pos)
{
	if (!update_iter(m->private, *pos, 1))
		return NULL;
	return m->private;
}

static const struct seq_operations kallmodsyms_op = {
	.start = s_mod_start,
	.next = s_mod_next,
	.stop = s_stop,
	.show = s_mod_show
};
#endif

static inline int kallsyms_for_perf(void)
{
#ifdef CONFIG_PERF_EVENTS
	extern int sysctl_perf_event_paranoid;
	if (sysctl_perf_event_paranoid <= 1)
		return 1;
#endif
	return 0;
}

/*
 * We show kallsyms information even to normal users if we've enabled
 * kernel profiling and are explicitly not paranoid (so kptr_restrict
 * is clear, and sysctl_perf_event_paranoid isn't set).
 *
 * Otherwise, require CAP_SYSLOG (assuming kptr_restrict isn't set to
 * block even that).
 */
bool kallsyms_show_value(const struct cred *cred)
{
	switch (kptr_restrict) {
	case 0:
		if (kallsyms_for_perf())
			return true;
		fallthrough;
	case 1:
		if (security_capable(cred, &init_user_ns, CAP_SYSLOG,
				     CAP_OPT_NOAUDIT) == 0)
			return true;
		fallthrough;
	default:
		return false;
	}
}

static int kallsyms_open_internal(struct inode *inode, struct file *file,
	const struct seq_operations *ops)
{
	/*
	 * We keep iterator in m->private, since normal case is to
	 * s_start from where we left off, so we avoid doing
	 * using get_symbol_offset for every symbol.
	 */
	struct kallsym_iter *iter;
	iter = __seq_open_private(file, ops, sizeof(*iter));
	if (!iter)
		return -ENOMEM;
	reset_iter(iter, 0);

	/*
	 * Instead of checking this on every s_show() call, cache
	 * the result here at open time.
	 */
	iter->show_value = kallsyms_show_value(file->f_cred);
	return 0;
}

static int kallsyms_open(struct inode *inode, struct file *file)
{
	return kallsyms_open_internal(inode, file, &kallsyms_op);
}

#ifdef CONFIG_KALLMODSYMS
static int kallmodsyms_open(struct inode *inode, struct file *file)
{
	return kallsyms_open_internal(inode, file, &kallmodsyms_op);
}
#endif

#ifdef	CONFIG_KGDB_KDB
const char *kdb_walk_kallsyms(loff_t *pos)
{
	static struct kallsym_iter kdb_walk_kallsyms_iter;
	if (*pos == 0) {
		memset(&kdb_walk_kallsyms_iter, 0,
		       sizeof(kdb_walk_kallsyms_iter));
		reset_iter(&kdb_walk_kallsyms_iter, 0);
	}
	while (1) {
		if (!update_iter(&kdb_walk_kallsyms_iter, *pos, 0))
			return NULL;
		++*pos;
		/* Some debugging symbols have no name.  Ignore them. */
		if (kdb_walk_kallsyms_iter.name[0])
			return kdb_walk_kallsyms_iter.name;
	}
}
#endif	/* CONFIG_KGDB_KDB */

static const struct proc_ops kallsyms_proc_ops = {
	.proc_open	= kallsyms_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= seq_release_private,
};

#ifdef CONFIG_KALLMODSYMS
static const struct proc_ops kallmodsyms_proc_ops = {
	.proc_open	= kallmodsyms_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= seq_release_private,
};
#endif

static int __init kallsyms_init(void)
{
	proc_create("kallsyms", 0444, NULL, &kallsyms_proc_ops);
#ifdef CONFIG_KALLMODSYMS
	proc_create("kallmodsyms", 0444, NULL, &kallmodsyms_proc_ops);
#endif
	return 0;
}
device_initcall(kallsyms_init);