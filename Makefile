LIBS   = -lclang
CFLAGS = -Wall -g

all: clang-tags

clang-tags: clang-tags.c
	cc $(CFLAGS) -o $@ $< $(LIBS) -std=gnu99

clean:
	rm -f clang-tags
