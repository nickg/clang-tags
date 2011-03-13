#define _GNU_SOURCE

#include "clang-c/Index.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include <ctype.h>
#include <getopt.h>
#include <dirent.h>
#include <regex.h>

typedef struct {
   CXFile     *source_file;
   const char *file_contents;
} VisitorArgs;

#define MAX_MISSING_FILES 100

static FILE   *output = NULL;
static char   *etags_buf = NULL;
static char   *etags_wr_ptr = NULL;
static size_t etags_buf_len = 0;
static char   *missing_files[MAX_MISSING_FILES] = { NULL };
static int    n_missing_files = 0;

#define SOURCE_REGEX "\\.(c|cpp|cc|cxx|h|hpp)$"

static void emit_file(const char *file)
{
   const size_t bytes = etags_wr_ptr - etags_buf;
   
   fprintf(output, "\x0c\n");
   fprintf(output, "%s,%lu\n", file, bytes);
   fputs(etags_buf, output);

   etags_wr_ptr = etags_buf;
}

static void emit_tag(const char *name, const char *text,
                     unsigned line, unsigned offset)
{
   const size_t etags_buf_off = etags_wr_ptr - etags_buf;
   size_t remain = etags_buf_len - etags_buf_off;
   const size_t max_line_off_sz = 64;
   const size_t line_len = strlen(name) + strlen(text) + max_line_off_sz;
   
   while (remain < line_len) {
      remain += etags_buf_len;
      etags_buf_len *= 2;
      etags_buf = realloc(etags_buf, etags_buf_len);
      assert(etags_buf != NULL);
      
      etags_wr_ptr = etags_buf + etags_buf_off;
   }

   etags_wr_ptr += snprintf(etags_wr_ptr, remain, "%s\x7f%s\x01%u,%u\n",
                            text, name, line, offset);
}

static bool end_search_char(char ch)
{
   return ch == ')' || ch == '{' || ch == '\n' || ch == '\r'
      || ch == ';';
}

static char *find_search_text(const char *content,
                              unsigned offset,
                              unsigned max)
{
   const char *start, *end;

   start = end = content + offset;

   // Search backwards to the beginning of the line
   while (start > content && start[-1] != '\n')
      start--;

   // Search forwards to the next brace or parenthesis
   while (end <= content + offset + max && !end_search_char(*end))
      end++;

   // Drop any trailing whitespace
   while (isspace(*end))
      end--;

   const size_t nchars = end - start + 1;
   char *buf = malloc(nchars + 1);
   assert(buf != NULL);

   memcpy(buf, start, nchars);
   buf[nchars] = '\0';

   return buf;
}

static enum CXChildVisitResult cursor_visitor(CXCursor cursor,
                                              CXCursor parent,
                                              CXClientData client_data)
{
   VisitorArgs *args = (VisitorArgs*)client_data;
   
   if (clang_isCursorDefinition(cursor)) {
      CXSourceLocation loc = clang_getCursorLocation(cursor);

      CXFile file;
      unsigned line, column, offset;
      clang_getSpellingLocation(loc, &file, &line, &column, &offset);

      if (file == args->source_file) {
         CXSourceRange extent = clang_getCursorExtent(cursor);
         unsigned end;
         clang_getSpellingLocation(clang_getRangeEnd(extent),
                                   NULL, NULL, NULL, &end);
         
         char *search = find_search_text(args->file_contents,
                                         offset, end - offset);
         
         CXString file_str = clang_getFileName(file);
         CXString str = clang_getCursorSpelling(cursor);

         if (clang_getCString(str)[0] != '\0') {
#if 0
            printf("visit: %s: line %u col %u off %u: %s\n",
                   clang_getCString(file_str), line, column, offset,
                   clang_getCString(str));
#endif
            emit_tag(clang_getCString(str), search, line, offset);
         }
         
         clang_disposeString(str);
         clang_disposeString(file_str);
         free(search);
      }
   }

   enum CXCursorKind kind = clang_getCursorKind(cursor);

   switch (kind) {
   default:
      return CXChildVisit_Continue;
   }
}

static void add_missing_file(char *file)
{
   assert(n_missing_files < MAX_MISSING_FILES);

   for (int i = 0; i < n_missing_files; i++) {
      if (strcmp(missing_files[i], file) == 0) {
         free(file);
         return;
      }         
   }

   missing_files[n_missing_files++] = file;
}

static void process_file(CXTranslationUnit tu, const char *file)
{
   static bool diag_reg_comp = false;
   static regex_t diag_reg;

   if (!diag_reg_comp) {
      int rc = regcomp(&diag_reg, "'(.*)' file not found", REG_EXTENDED);
      assert(rc == 0);

      diag_reg_comp = true;
   }
   
   unsigned ndiag = clang_getNumDiagnostics(tu);

   for (unsigned i = 0; i < ndiag; i++) {
      CXDiagnostic diag = clang_getDiagnostic(tu, i);

      unsigned cat = clang_getDiagnosticCategory(diag);
      if (cat == 2 && n_missing_files < MAX_MISSING_FILES) {
         // Preprocessor issue
         CXString spell = clang_getDiagnosticSpelling(diag);

         regmatch_t rm[2];
         const char *s = clang_getCString(spell);
         if (regexec(&diag_reg, s, 2, rm, 0) != REG_NOMATCH) {
            char *buf = malloc(rm[1].rm_eo - rm[1].rm_so + 2);
            const char *src = s + rm[1].rm_so;
            char *dst = buf;
            while (src != s + rm[1].rm_eo)
               *dst++ = *src++;
            *dst = '\0';

            add_missing_file(buf);
         }
         
         clang_disposeString(spell);
      }
      
      clang_disposeDiagnostic(diag);
   }
   
   int fd = open(file, O_RDONLY);
   if (fd < 0) {
      perror(file);
      return;
   }

   struct stat st;
   if (fstat(fd, &st) < 0) {
      perror(file);
      return;
   }

   char *contents = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
   if (contents == MAP_FAILED) {
      perror("mmap");
      return;
   }
   
   CXCursor cur = clang_getTranslationUnitCursor(tu);

   VisitorArgs args = {
      .source_file   = clang_getFile(tu, file),
      .file_contents = contents
   };
   clang_visitChildren(cur, cursor_visitor, (CXClientData*)&args);

   emit_file(file);

   munmap(contents, st.st_size);
   close(fd);
}

static void visit_path(const char *path, regex_t *preg,
                       CXIndex index, const char *clang_argv[], int clang_argc)
{
   struct stat st;
   int rc = stat(path, &st);
   if (rc < 0) {
      perror(path);
      return;
   }

   if (S_ISDIR(st.st_mode)) {
      DIR *d = opendir(path);
      if (d == NULL) {
         perror(path);
         return;
      }

      struct dirent *ent;
      while ((ent = readdir(d)) != NULL) {
         if (ent->d_name[0] != '.') {
            char *newpath;
            asprintf(&newpath, "%s/%s", path, ent->d_name);
            assert(newpath != NULL);

            visit_path(newpath, preg, index, clang_argv, clang_argc);
            
            free(newpath);
         }
      }

      closedir(d);
   }
   else if (S_ISREG(st.st_mode)) {
      if (regexec(preg, path, 0, NULL, 0) != REG_NOMATCH) {
         static int nfiles = 0;
         printf(".");
         if ((++nfiles % 10) == 0)
            printf("%d", nfiles);
         fflush(stdout);

         CXTranslationUnit tu = clang_parseTranslationUnit(
            index,            // Index
            path,             // Source file name
            clang_argv,       // Command line arguments
            clang_argc,       // Number of arguments
            NULL,             // Unsaved files
            0,                // Number of unsaved files
            0);               // Flags

         process_file(tu, path);
         clang_disposeTranslationUnit(tu); 
      }
   }
   else if (S_ISLNK(st.st_mode)) {
      // Ignore symlinks for now
   }
}

static struct option long_options[] = {
   {0, 0, 0, 0}
};

int main(int argc, char **argv)
{
   const size_t max_clang_args = 64;
   int clang_argc = 0;
   char **clang_argv = malloc(sizeof(char*) * max_clang_args);
   assert(clang_argv != NULL);
   
   const char *spec = "I:";
   int c, failure = 0;
   while ((c = getopt_long(argc, argv, spec, long_options, NULL)) != -1) {
      switch (c) {
      case '0':    // Set a flag
         break;
      case 'I':
         if (clang_argc < max_clang_args - 1) {
            clang_argv[clang_argc++] = "-I";
            clang_argv[clang_argc++] = optarg;
         }
         break;
      case '?':
         failure = 1;
         break;
      default:
         abort();
      }
   }
   if (failure)
      return EXIT_FAILURE;

   output = fopen("TAGS", "w");
   if (output == NULL) {
      fprintf(stderr, "failed to open TAGS for writing\n");
      return EXIT_FAILURE;
   }

   etags_buf_len = 16;
   etags_buf = etags_wr_ptr = malloc(etags_buf_len);
   assert(etags_buf != NULL);
   
   CXIndex index = clang_createIndex(
      1,    // excludeDeclarationsFromPCH 
      0);   // displayDiagnostics

   regex_t source_files;
   int rc = regcomp(&source_files, SOURCE_REGEX, REG_EXTENDED);
   assert(rc == 0);
   
   for (int i = optind; i < argc; i++) {
      visit_path(argv[i], &source_files, index,
                 (const char**)clang_argv, clang_argc);
   }
   
   printf("\nDone\n");

   if (n_missing_files > 0) {
      printf("\nThe following include files could not be found:\n");
      for (int i = 0; i < n_missing_files; i++)
         printf("   %s\n", missing_files[i]);
      printf("Using -I to specify header search directories will "
             "improve results.\n");
   }

   clang_disposeIndex(index);
        
   fclose(output);
   
   free(etags_buf);
   free(clang_argv);
   regfree(&source_files);

   return 0;
}
