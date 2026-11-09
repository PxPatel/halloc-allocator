#ifndef HALLOC_H
#define HALLOC_H

#include <stddef.h>

void *halloc(size_t n);
void hfree(void *p);
void hshow();

#endif