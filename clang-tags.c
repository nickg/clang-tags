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

typedef struct {
   CXFile     *source_file;
   const char *file_contents;
} VisitorArgs;

static FILE   *output = NULL;
static char   *etags_buf = NULL;
static char   *etags_wr_ptr = NULL;
static size_t etags_buf_len = 0;

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

      printf("*** resize buffer to %lu\n", etags_buf_len);

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
         printf("visit: %s: line %u col %u off %u: %s\n",
                clang_getCString(file_str), line, column, offset,
                clang_getCString(str));
         emit_tag(clang_getCString(str), search, line, offset);
         
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

static void process_file(CXTranslationUnit tu, const char *file)
{
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
 
int main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: clang-tags [file]\n");
      return EXIT_FAILURE;
   }

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
      1);   // displayDiagnostics

   CXTranslationUnit tu = clang_parseTranslationUnit(
      index,            // Index
      argv[1],          // Source file name
      NULL,             // Command line arguments
      0,                // Number of arguments
      NULL,             // Unsaved files
      0,                // Number of unsaved files
      0);               // Flags

   process_file(tu, argv[1]);
   
   fclose(output);
   
   free(etags_buf);
   clang_disposeTranslationUnit(tu);
   clang_disposeIndex(index);
        
   return 0;
}
