/** Copyright 2011-2013 Thorsten Wißmann. All rights reserved.
 *
 * This software is licensed under the "Simplified BSD License".
 * See LICENSE for details */

#ifndef __HERBST_UTILS_H_
#define __HERBST_UTILS_H_

#include "glib-backports.h"
#include <stddef.h>
#include <stdbool.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#define LENGTH(X) (sizeof(X)/sizeof(*X))
#define SHIFT(ARGC, ARGV) (--(ARGC) && ++(ARGV))
#define MOD(X, N) ((((X) % (signed)(N)) + (signed)(N)) % (signed)(N))

#define container_of(ptr, type, member) \
    ((type *)( (char *)(ptr)- offsetof(type,member) ))


/// print a printf-like message to stderr and exit
void die(const char *errstr, ...);

// get X11 color from color string
unsigned long getcolor(const char *colstr);

#define ATOM(A) XInternAtom(g_display, (A), False)

GString* window_property_to_g_string(Display* dpy, Window window, Atom atom);
GString* window_class_to_g_string(Display* dpy, Window window);
GString* window_instance_to_g_string(Display* dpy, Window window);
int window_pid(Display* dpy, Window window);

typedef void* HSTree;
struct HSTreeInterface;
typedef struct HSTreeInterface {
    struct HSTreeInterface  (*nth_child)(HSTree root, size_t idx);
    size_t                  (*child_count)(HSTree root);
    void                    (*append_caption)(HSTree root, GString* output);
    HSTree                  data;
    void                    (*destructor)(HSTree data); /* how to free the data tree */
} HSTreeInterface;

void tree_print_to(HSTreeInterface* intface, GString* output);


bool is_herbstluft_window(Display* dpy, Window window);

bool is_window_mapable(Display* dpy, Window window);
bool is_window_mapped(Display* dpy, Window window);

bool window_has_property(Display* dpy, Window window, char* prop_name);

bool string_to_bool_error(char* string, bool oldvalue, bool* error);
bool string_to_bool(char* string, bool oldvalue);

char* strlasttoken(char* str, char* delim);

time_t get_monotonic_timestamp();

// duplicates an argument-vector
char** argv_duplicate(int argc, char** argv);
// frees all entries in argument-vector and then the vector itself
void argv_free(int argc, char** argv);

XRectangle parse_rectangle(char* string);

void g_queue_remove_element(GQueue* queue, GList* elem);

// find an element in an array buf with elems elements of size size.
int array_find(void* buf, size_t elems, size_t size, void* needle);
void array_reverse(void* buf, size_t elems, size_t size);

int min(int a, int b);

// utils for tables
typedef bool (*MemberEquals)(void* pmember, void* needle);
bool memberequals_string(void* pmember, void* needle);
bool memberequals_int(void* pmember, void* needle);

void* table_find(void* start, size_t elem_size, size_t count,
                 size_t member_offset, MemberEquals equals, void* needle);

void set_window_double_border(Display *dpy, Window win, int ibw,
                              unsigned long inner_color, unsigned long outer_color);

#define STATIC_TABLE_FIND(TYPE, TABLE, MEMBER, EQUALS, NEEDLE)  \
    ((TYPE*) table_find((TABLE),                                \
                        sizeof(TABLE[0]),                       \
                        LENGTH((TABLE)),                        \
                        offsetof(TYPE, MEMBER),                 \
                        EQUALS,                                 \
                        (NEEDLE)))

#define STATIC_TABLE_FIND_STR(TYPE, TABLE, MEMBER, NEEDLE)  \
    STATIC_TABLE_FIND(TYPE, TABLE, MEMBER, memberequals_string, NEEDLE)

#define INDEX_OF(ARRAY, PELEM) \
    (((char*)(PELEM) - (char*)(ARRAY)) / (sizeof (*ARRAY)))

// returns the unichar in GSTR at position GSTR
#define UTF8_STRING_AT(GSTR, OFFS) \
    g_utf8_get_char( \
        g_utf8_offset_to_pointer((GSTR), (OFFS))) \

#define RECTANGLE_EQUALS(a, b) (\
        (a).x == (b).x &&   \
        (a).y == (b).y &&   \
        (a).width == (b).width &&   \
        (a).height == (b).height    \
    )

// returns an posix sh escaped string or NULL if there is nothing to escape
// if a new string is returned, then the caller has to free it
char* posix_sh_escape(char* source);
// does the reverse action to posix_sh_escape by modifing the string
void posix_sh_compress_inplace(char* str);

#endif

