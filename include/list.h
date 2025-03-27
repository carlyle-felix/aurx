#ifndef LIST_H
#define LIST_H

#include <stdbool.h>

typedef struct node {
    char *pkgname;
    char *pkgver;
    bool update;
    struct node *next;
} List;

List *struct_malloc();
List *add_pkg(List *pkglist, char *pkgname, char *pkgver);
List *remove_pkg(List *pkglist, char *pkgname);
List *find_pkg(List *pkglist, char *pkgname);
void clear_list(List *pkglist);


#endif