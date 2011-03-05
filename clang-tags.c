#include "clang-c/Index.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
   CXFile *source_file;
} VisitorArgs;

static void emit_file(const char *file)
{

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
         CXString file_str = clang_getFileName(file);
         CXString str = clang_getCursorSpelling(cursor);
         printf("visit: %s: line %u col %u off %u: %s\n",
                clang_getCString(file_str), line, column, offset,
                clang_getCString(str));
         clang_disposeString(str);
         clang_disposeString(file_str);
      }
   }

   enum CXCursorKind kind = clang_getCursorKind(cursor);

   switch (kind) {
   default:
      return CXChildVisit_Continue;
   }
}
 
int main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: clang-tags [file]\n");
      exit(EXIT_FAILURE);
   }
   
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

   printf("tu = %p\n", tu);

   CXCursor cur = clang_getTranslationUnitCursor(tu);

   VisitorArgs args = {
      .source_file = clang_getFile(tu, argv[1])
   };
   clang_visitChildren(cur, cursor_visitor, (CXClientData*)&args);
   
   clang_disposeTranslationUnit(tu);

   clang_disposeIndex(index);
   return 0;
}
