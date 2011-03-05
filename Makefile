LIBS   = -lclang
CFLAGS = -Wall -g

all: clang-tags

clang-tags: clang-tags.c
	clang $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f clang-tags
