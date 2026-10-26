#ifndef VMFC_OBJC_UTIL_H
#define VMFC_OBJC_UTIL_H

#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *);

typedef struct {
    double x, y, w, h;
} vmfc_rect;

typedef struct {
    double w, h;
} vmfc_size;

static inline id o_cls(const char *name)
{
    return (id)objc_getClass(name);
}

static inline SEL o_sel(const char *name)
{
    return sel_registerName(name);
}

static inline id o_alloc(const char *cls)
{
    return ((id(*)(id, SEL))objc_msgSend)(o_cls(cls), o_sel("alloc"));
}

static inline id o_init(const char *cls)
{
    return ((id(*)(id, SEL))objc_msgSend)(o_alloc(cls), o_sel("init"));
}

static inline id o_msg0(id o, const char *s)
{
    return ((id(*)(id, SEL))objc_msgSend)(o, o_sel(s));
}

static inline id o_msg1(id o, const char *s, id a)
{
    return ((id(*)(id, SEL, id))objc_msgSend)(o, o_sel(s), a);
}

static inline void o_vmsg0(id o, const char *s)
{
    ((void(*)(id, SEL))objc_msgSend)(o, o_sel(s));
}

static inline void o_vmsg1(id o, const char *s, id a)
{
    ((void(*)(id, SEL, id))objc_msgSend)(o, o_sel(s), a);
}

static inline id o_msg2(id o, const char *s, id a, id b)
{
    return ((id(*)(id, SEL, id, id))objc_msgSend)(o, o_sel(s), a, b);
}

static inline void o_vmsg2(id o, const char *s, id a, id b)
{
    ((void(*)(id, SEL, id, id))objc_msgSend)(o, o_sel(s), a, b);
}

static inline id o_msg3(id o, const char *s, id a, id b, id c)
{
    return ((id(*)(id, SEL, id, id, id))objc_msgSend)(o, o_sel(s), a, b, c);
}

static inline void o_vmsg3(id o, const char *s, id a, id b, id c)
{
    ((void(*)(id, SEL, id, id, id))objc_msgSend)(o, o_sel(s), a, b, c);
}

static inline void o_set_ulong(id o, const char *s, unsigned long v)
{
    ((void(*)(id, SEL, unsigned long))objc_msgSend)(o, o_sel(s), v);
}

static inline void o_set_u64(id o, const char *s, uint64_t v)
{
    ((void(*)(id, SEL, uint64_t))objc_msgSend)(o, o_sel(s), v);
}

static inline void o_set_long(id o, const char *s, long v)
{
    ((void(*)(id, SEL, long))objc_msgSend)(o, o_sel(s), v);
}

static inline void o_set_bool(id o, const char *s, BOOL v)
{
    ((void(*)(id, SEL, BOOL))objc_msgSend)(o, o_sel(s), v);
}

static inline BOOL o_bool(id o, const char *s)
{
    return ((BOOL(*)(id, SEL))objc_msgSend)(o, o_sel(s));
}

static inline BOOL o_bool_err(id o, const char *s, id *err)
{
    return ((BOOL(*)(id, SEL, id *))objc_msgSend)(o, o_sel(s), err);
}

static inline unsigned long o_ulong(id o, const char *s)
{
    return ((unsigned long(*)(id, SEL))objc_msgSend)(o, o_sel(s));
}

static inline id o_str(const char *s)
{
    if (!s)
        return NULL;
    return o_msg1(o_cls("NSString"), "stringWithUTF8String:", (id)s);
}

static inline const char *o_cstr(id s)
{
    if (!s)
        return "";
    return ((const char *(*)(id, SEL))objc_msgSend)(s, o_sel("UTF8String"));
}

static inline id o_url(const char *p)
{
    return o_msg1(o_cls("NSURL"), "fileURLWithPath:", o_str(p));
}

static inline id o_data(const void *bytes, size_t len)
{
    return ((id(*)(id, SEL, const void *, unsigned long))objc_msgSend)(
        o_cls("NSData"), o_sel("dataWithBytes:length:"), bytes, (unsigned long)len);
}

static inline id o_array(id *items, size_t n)
{
    return ((id(*)(id, SEL, const id *, unsigned long))objc_msgSend)(
        o_cls("NSArray"), o_sel("arrayWithObjects:count:"), items, (unsigned long)n);
}

static inline id o_empty_array(void)
{
    return o_msg0(o_cls("NSArray"), "array");
}

static inline const void *o_bytes(id data)
{
    return ((const void *(*)(id, SEL))objc_msgSend)(data, o_sel("bytes"));
}

static inline unsigned long o_len(id data)
{
    return ((unsigned long(*)(id, SEL))objc_msgSend)(data, o_sel("length"));
}

#endif
