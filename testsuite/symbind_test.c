/*
This file is part of Spindle.  For copyright information see the COPYRIGHT 
file in the top level directory, or at 
https://github.com/hpc/Spindle/blob/master/COPYRIGHT

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License (as published by the Free Software
Foundation) version 2.1 dated February 1999.  This program is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY; without even the IMPLIED
WARRANTY OF MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms 
and conditions of the GNU Lesser General Public License for more details.  You should 
have received a copy of the GNU Lesser General Public License along with this 
program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#define _GNU_SOURCE
#include <link.h>
#include <stdio.h>
#include <elf.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <assert.h>

const char *executable = "executable";
const char *unknown_dso = "[unknown dso]";

#define MATCHED_NOT_FOUND 0
#define MATCHED_SUCCESS 1
#define MATCHED_TARGET_MISMATCH -1

typedef struct {
   const char *symname;
   const char *altname;
   const char *libtarget;
   const char *libmatched;
   void *got_ptr;
   int matched;
} test_bindings_t;

typedef struct {
   const char *dso_name;
   int is_exe;
   const ElfW(Phdr) *phdr;
   ElfW(Addr) load_addr;
   int num_phdrs;
} get_phdr_for_t;

int verbose = 0;

static int get_phdr_for_cb(struct dl_phdr_info *info, size_t size, void *data)
{
   get_phdr_for_t *params;

   params = (get_phdr_for_t *) data;
   
   if ((info->dlpi_name[0] == '\0' && params->is_exe) ||
       (strstr(info->dlpi_name, params->dso_name)))
   {
      params->phdr = info->dlpi_phdr;
      params->load_addr = info->dlpi_addr;
      params->num_phdrs = (int) info->dlpi_phnum;
      return 1;
   }
   return 0;
}

static int get_phdr_for(const char *dso_name, ElfW(Phdr) **phdr, ElfW(Addr) *base, int *num_phdrs)
{
   get_phdr_for_t pinfo;

   pinfo.dso_name = dso_name;
   pinfo.is_exe = (dso_name == executable);
   pinfo.phdr = NULL;

   dl_iterate_phdr(get_phdr_for_cb, &pinfo);
   if (pinfo.phdr) {
      *phdr = (ElfW(Phdr) *) pinfo.phdr;
      *base = pinfo.load_addr;
      *num_phdrs = pinfo.num_phdrs;
      return 0;
   }

   return -1;
}
   
typedef struct {
   ElfW(Addr) addr;
   char *name;
} find_dso_t;

static int check_dso_cb(struct dl_phdr_info *info, size_t size, void *data)
{
   find_dso_t *params;
   int i;
   ElfW(Addr) addr, start, end;

   params = (find_dso_t *) data;
   addr = params->addr;
   
   for (i = 0; i < info->dlpi_phnum; i++) {
      const ElfW(Phdr) *phdr = info->dlpi_phdr + i;
      if (phdr->p_type != PT_LOAD)
         continue;
      start = phdr->p_vaddr + info->dlpi_addr;
      end = start + phdr->p_memsz;
      if (addr >= start && addr < end) {
         if (info->dlpi_name[0] != '\0')
            params->name = (char *) info->dlpi_name;
         else
            params->name = (char *) "[EXE]";
         return 1;
      }
   }
   return 0;
}

static int dso_at_address(ElfW(Addr) addr, char **name)
{
   find_dso_t find_dso;
   find_dso.addr = addr;
   find_dso.name = NULL;

   dl_iterate_phdr(check_dso_cb, &find_dso);
   if (!find_dso.name) {
      return -1;
   }

   *name = find_dso.name;
   return 0;
}

#if defined(__x86_64)
#define REL_TYPE ElfW(Rela)
#else
#error Need rel type for architecture
#endif

static int check_relocations(REL_TYPE *rels, unsigned long num_rels, ElfW(Addr) base, ElfW(Sym) *symtable, char *strtable, test_bindings_t *test)
{
   unsigned long i;
   for (i = 0; i < num_rels; i++) {
      REL_TYPE *r;
      ElfW(Sym) *sym;
      unsigned long symidx;
      char *name;
      ElfW(Addr) address;
      char *target_dso_name = 0;
      int result, test_i;
      
      r = rels + i;

      if (sizeof(void*) == 8) 
         symidx = ELF64_R_SYM(r->r_info);
      else
         symidx = ELF32_R_SYM(r->r_info);
      sym = symtable + symidx;
      name = strtable + sym->st_name;

      for (test_i = 0; test[test_i].symname; test_i++) {
         if (strcmp(name, "dlopen") == 0)
            continue;
         if (strstr(name, test[test_i].symname) || (test[test_i].altname && strstr(name, test[test_i].altname))) {
            break;
         }
      }
      if (!test[test_i].symname)
         continue;

      test[test_i].got_ptr = (void*) (r->r_offset + base);
      address = *((ElfW(Addr)*) test[test_i].got_ptr);

      result = dso_at_address(address, &target_dso_name);
      if (result == -1) {
         target_dso_name = (char *) unknown_dso;
      }
      test[test_i].libmatched = target_dso_name;
      if (strstr(target_dso_name, test[test_i].libtarget)) {
         test[test_i].matched = MATCHED_SUCCESS;
      }
      else {
         if (verbose) {
            printf("%s mismatch. index = %lu; rels = %p; got = %p; target = 0x%lx; target_lib = %s\n", name, i, rels, test[test_i].got_ptr, address, target_dso_name);
         }
         test[test_i].matched = MATCHED_TARGET_MISMATCH;
      }
   }
   return 0;
}

static int validate(test_bindings_t *test, const char *libname)
{
   int i;
   int had_error = 0;
   for (i = 0; test[i].symname != NULL; i++) {
      if (test[i].matched == MATCHED_NOT_FOUND) {
         printf("FAIL: %s: Symbol not found in %s\n", test[i].symname, libname);
         had_error = 1;
      }
      if (test[i].matched == MATCHED_SUCCESS) {
         if (verbose)
            printf("PASS: %s bound to %s in %s\n", test[i].symname, test[i].libmatched, libname);
      }
      if (test[i].matched == MATCHED_TARGET_MISMATCH) {
         printf("FAIL: %s: Symbol incorrectly bound to %s in %s\n", test[i].symname, test[i].libmatched, libname);
         had_error = 1;
      }      
   }
   return had_error ? -1 : 0;
}

static int check_bindings(const char *dso_name, test_bindings_t *test)
{
   int result;
   ElfW(Phdr) *phdr = NULL;
   ElfW(Addr) base = 0;
   ElfW(Dyn) *dynamic = NULL;
   REL_TYPE *rel_dyn = NULL;
   unsigned long num_rel_dyn = 0;
   REL_TYPE *rel_jmp = NULL;
   unsigned long num_rel_jmp = 0;
   ElfW(Sym) *symtable = NULL;
   char *strtable = NULL;
   
   int num_phdrs, i;

   result = get_phdr_for(dso_name, &phdr, &base, &num_phdrs);
   if (result == -1) {
      fprintf(stderr, "Error getting phdr for DSO %s\n", dso_name);
      return -1;
   }

   for (i = 0; i < num_phdrs; i++) {
      if (phdr[i].p_type == PT_DYNAMIC) {
         dynamic = (ElfW(Dyn) *) (base + phdr[i].p_vaddr);
         break;
      }
   }
   if (!dynamic) {
      fprintf(stderr, "Could not find dynamic section in %s\n", dso_name);
      return -1;
   }

   for (i = 0; dynamic[i].d_tag != DT_NULL; i++) {
      if (dynamic[i].d_tag == DT_REL || dynamic[i].d_tag == DT_RELA) {
         rel_dyn = (REL_TYPE *) dynamic[i].d_un.d_ptr;
      }
      else if (dynamic[i].d_tag == DT_RELSZ || dynamic[i].d_tag == DT_RELASZ) {
         num_rel_dyn = dynamic[i].d_un.d_val / sizeof(REL_TYPE);
      }
      else if (dynamic[i].d_tag == DT_JMPREL) {
         rel_jmp = (REL_TYPE *) dynamic[i].d_un.d_ptr;
      }
      else if (dynamic[i].d_tag == DT_PLTRELSZ) {
         num_rel_jmp = dynamic[i].d_un.d_val / sizeof(REL_TYPE);
      }
      else if (dynamic[i].d_tag == DT_SYMTAB) {
         symtable = (ElfW(Sym) *) dynamic[i].d_un.d_ptr;
      }
      else if (dynamic[i].d_tag == DT_STRTAB) {
         strtable = (char *) dynamic[i].d_un.d_ptr;
      }
   }

   if (!symtable || !strtable) {
      fprintf(stderr, "Could not load symbol table from DSO %s\n", dso_name);
      return -1;
   }
   if (rel_dyn && num_rel_dyn) {
      result = check_relocations(rel_dyn, num_rel_dyn, base, symtable, strtable, test);
      if (result == -1) {
         return -1;
      }
   }
   if (rel_jmp && num_rel_jmp) {
      result = check_relocations(rel_jmp, num_rel_jmp, base, symtable, strtable, test);
      if (result == -1) {
         return -1;
      }
   }

   return validate(test, dso_name);
}

#define TDL_NO_DLOPEN       (1 << 0)
#define TDL_DLOPEN          (1 << 1)
#define TDL_BIND_NOW        (1 << 2)
#define TDL_NO_STAT         (1 << 3)
#define TDL_NO_DLSYM        (1 << 4)

void *runtest_for_lib(const char *libname, int options)
{
   int result;
   const char *libspindle;
   void *dl;
   if (getenv("SPINDLE"))
      libspindle = unknown_dso;
   else
      libspindle = "libc.so";

   test_bindings_t test[] = {
      { "getpid", NULL, "libc.so", NULL, NULL, 0 },
      { "readlink", NULL, libspindle, NULL, NULL, 0 },
      { "open", NULL, libspindle, NULL, NULL, 0 },
      { "read", NULL, "libc.so", NULL, NULL, 0 },
      { "fxstat", "fstat", libspindle, NULL, NULL, 0 },
      { "lxstat", "lstat", libspindle, NULL, NULL, 0 },
      { "xstat", "stat", libspindle, NULL, NULL, 0 },
      { NULL, NULL, NULL, NULL, 0 }
   };

   if (options & TDL_NO_STAT) {
      for (int i = 0; test[i].symname != NULL; i++) {
         if (strstr(test[i].symname, "stat"))
            memset(test+i, 0, sizeof(test_bindings_t));
      }
   }

   if (options & TDL_DLOPEN) {
      dl = dlopen(libname, options & TDL_BIND_NOW ? RTLD_NOW : RTLD_LAZY);
      if (!dl) {
         fprintf(stderr, "Failed to load library %s: %s\n", libname, dlerror());
         return NULL;
      }
      if (!(options & TDL_NO_DLSYM)) {
         int (*fptr)(void) = dlsym(dl, "dowork");
         if (!fptr) {
            fprintf(stderr, "Library %s does not export dowork\n", libname);
            return NULL;
         }
         fptr();
      }
   }
   else {
      dl = (void*) 1;
   }
      
   result = check_bindings(libname, test);
   if (result == -1) {
      fprintf(stderr, "Error in %s\n", libname);
      return NULL;
   }
   return dl;
}

#include "symbind_fileops.c"

#define NUM_TESTS 8
int main(int argc, char *argv[]) {
   int result, had_error = 0, cur = 0;
   void *libresult[NUM_TESTS];

   if (argc > 1 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--verbose") == 0))
      verbose = 1;
   result = do_file_ops();
   if (result == -1) {
      fprintf(stderr, "ERROR: Unexpected error return from file ops\n");
      return -1;
   }
   
   libresult[cur] = runtest_for_lib(executable, TDL_NO_DLOPEN);
   if (!libresult[cur])
      had_error = 1;
   cur++;   
   libresult[cur] = runtest_for_lib("libsymbind_a.so", TDL_NO_DLOPEN);
   if (!libresult[cur])
      had_error = 1;
   cur++;   
   libresult[cur] = runtest_for_lib("libsymbind_b.so", TDL_NO_DLOPEN);
   if (!libresult[cur])
      had_error = 1;
   cur++;   
   libresult[cur] = runtest_for_lib("libsymbind_c.so", TDL_DLOPEN);
   if (!libresult[cur])
      had_error = 1;
   cur++;   
   libresult[cur] = runtest_for_lib("libsymbind_d.so", TDL_DLOPEN);
   if (!libresult[cur])
      had_error = 1;
   cur++;   
   libresult[cur] = runtest_for_lib("libsymbind_e.so", TDL_DLOPEN | TDL_BIND_NOW);
   if (!libresult[cur])
      had_error = 1;   
   cur++;   
   libresult[cur] = runtest_for_lib("libsymbind_f.so", TDL_DLOPEN);
   if (!libresult[cur])
      had_error = 1;
   cur++;   
   libresult[cur] = runtest_for_lib("libsymbind_g.so", TDL_DLOPEN | TDL_NO_STAT | TDL_NO_DLSYM);
   if (!libresult[cur])
      had_error = 1;
   cur++;   
   
   assert(cur == NUM_TESTS);

   for (cur = 0; cur < NUM_TESTS; cur++) {
      if (libresult[cur] && libresult[cur] != (void*) 1)
         dlclose(libresult[cur]);
   }
   
   cur = 0;
   libresult[cur] = runtest_for_lib("libsymbind_e.so", TDL_DLOPEN);
   if (!libresult[cur])
      had_error = 1;

   if (had_error)
      printf("Failed\n");
   else
      printf("PASSED\n");
   return had_error ? -1 : 0;
}
