/* advapi32 registry: an in-memory key tree under the predefined roots. Nothing persists past the page; the Gameforge
   InstallationId, which the client sends at login, is seeded at startup from NC_INSTALLATION_ID. */
#include "port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define RET(v) (c->eax = (uint32_t)(v))

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_INVALID_HANDLE 6
#define ERROR_INVALID_PARAMETER 87
#define ERROR_MORE_DATA 234
#define REG_SZ 1
#define REG_CREATED_NEW_KEY 1
#define REG_OPENED_EXISTING_KEY 2

#define HKEY_ROOT_FIRST 0x80000000u
#define HKEY_ROOT_COUNT 7
#define HKEY_LOCAL_MACHINE 0x80000002u
#define KEY_HANDLE_BASE 0x00410000u
#define MAX_KEY_HANDLES 256

typedef struct Value {
    char* name;
    uint32_t type, size;
    uint8_t* data;
    struct Value* next;
} Value;

typedef struct Key {
    char* name;
    struct Key *child, *sibling;
    Value* values;
} Key;

static Key roots[HKEY_ROOT_COUNT];
static Key* open_keys[MAX_KEY_HANDLES];

static Key* key_of(uint32_t h) {
    if (h >= HKEY_ROOT_FIRST && h < HKEY_ROOT_FIRST + HKEY_ROOT_COUNT) return &roots[h - HKEY_ROOT_FIRST];
    uint32_t i = (h - KEY_HANDLE_BASE) / 4;
    if (h < KEY_HANDLE_BASE || (h - KEY_HANDLE_BASE) % 4 || i >= MAX_KEY_HANDLES) return 0;
    return open_keys[i];
}

static uint32_t handle_for(Key* k) {
    for (uint32_t i = 0; i < MAX_KEY_HANDLES; i++)
        if (!open_keys[i]) {
            open_keys[i] = k;
            return KEY_HANDLE_BASE + 4 * i;
        }
    return 0;
}

static Key* child_of(Key* parent, const char* name, size_t len, int create) {
    for (Key* k = parent->child; k; k = k->sibling)
        if (strlen(k->name) == len && !strncasecmp(k->name, name, len)) return k;
    if (!create) return 0;
    Key* k = calloc(1, sizeof *k);
    k->name = strndup(name, len);
    k->sibling = parent->child;
    parent->child = k;
    return k;
}

/* Walks a backslash-separated path; *created reports whether the last component had to be made. */
static Key* walk(Key* from, const char* path, int create, int* created) {
    if (created) *created = 0;
    while (path && *path) {
        while (*path == '\\') path++;
        if (!*path) break;
        size_t len = strcspn(path, "\\");
        Key* next = child_of(from, path, len, 0);
        if (!next && create) {
            next = child_of(from, path, len, 1);
            if (created) *created = 1;
        }
        if (!next) return 0;
        from = next;
        path += len;
    }
    return from;
}

static Value* value_of(Key* k, const char* name) {
    for (Value* v = k->values; v; v = v->next)
        if (!strcasecmp(v->name, name)) return v;
    return 0;
}

static void value_set(Key* k, const char* name, uint32_t type, const void* data, uint32_t size) {
    Value* v = value_of(k, name);
    if (!v) {
        v = calloc(1, sizeof *v);
        v->name = strdup(name);
        v->next = k->values;
        k->values = v;
    }
    free(v->data);
    v->type = type;
    v->size = size;
    v->data = size ? malloc(size) : 0;
    if (size) memcpy(v->data, data, size);
}

static const char* guest_str(uint32_t p) { return p ? GSTR(p) : ""; }

static void a_RegOpenKeyExA(CPU* c) {
    Key* from = key_of(ARG(0));
    if (!from) { RET(ERROR_INVALID_HANDLE); return; }
    if (!ARG(4)) { RET(ERROR_INVALID_PARAMETER); return; }
    Key* k = walk(from, guest_str(ARG(1)), 0, 0);
    if (!k) { RET(ERROR_FILE_NOT_FOUND); return; }
    uint32_t h = handle_for(k);
    if (!h) { RET(ERROR_INVALID_HANDLE); return; }
    M32(ARG(4)) = h;
    RET(0);
}

static void a_RegCreateKeyExA(CPU* c) {
    Key* from = key_of(ARG(0));
    if (!from) { RET(ERROR_INVALID_HANDLE); return; }
    if (!ARG(7)) { RET(ERROR_INVALID_PARAMETER); return; }
    int created;
    Key* k = walk(from, guest_str(ARG(1)), 1, &created);
    uint32_t h = handle_for(k);
    if (!h) { RET(ERROR_INVALID_HANDLE); return; }
    M32(ARG(7)) = h;
    if (ARG(8)) M32(ARG(8)) = created ? REG_CREATED_NEW_KEY : REG_OPENED_EXISTING_KEY;
    RET(0);
}

static void a_RegCloseKey(CPU* c) {
    uint32_t h = ARG(0);
    if (!key_of(h)) { RET(ERROR_INVALID_HANDLE); return; }
    if (h >= KEY_HANDLE_BASE) open_keys[(h - KEY_HANDLE_BASE) / 4] = 0;
    RET(0);
}

static void a_RegFlushKey(CPU* c) { RET(key_of(ARG(0)) ? 0 : ERROR_INVALID_HANDLE); }

static void a_RegQueryValueExA(CPU* c) {
    Key* k = key_of(ARG(0));
    if (!k) { RET(ERROR_INVALID_HANDLE); return; }
    uint32_t data = ARG(4), size = ARG(5);
    if (data && !size) { RET(ERROR_INVALID_PARAMETER); return; }
    Value* v = value_of(k, guest_str(ARG(1)));
    if (!v) { RET(ERROR_FILE_NOT_FOUND); return; }
    if (ARG(3)) M32(ARG(3)) = v->type;
    if (!size) { RET(0); return; }
    uint32_t have = M32(size);
    M32(size) = v->size;
    if (!data) { RET(0); return; }
    if (have < v->size) { RET(ERROR_MORE_DATA); return; }
    memcpy(GPTR(data), v->data, v->size);
    RET(0);
}

static void a_RegSetValueExA(CPU* c) {
    Key* k = key_of(ARG(0));
    if (!k) { RET(ERROR_INVALID_HANDLE); return; }
    uint32_t type = ARG(3), data = ARG(4), size = ARG(5);
    if (size && !data) { RET(ERROR_INVALID_PARAMETER); return; }
    value_set(k, guest_str(ARG(1)), type, data ? GPTR(data) : 0, size);
    RET(0);
}

static const Hle reg_table[] = {
    {"advapi32.dll", "RegOpenKeyExA", 5, a_RegOpenKeyExA},
    {"advapi32.dll", "RegCreateKeyExA", 9, a_RegCreateKeyExA},
    {"advapi32.dll", "RegCloseKey", 1, a_RegCloseKey},
    {"advapi32.dll", "RegFlushKey", 1, a_RegFlushKey},
    {"advapi32.dll", "RegQueryValueExA", 6, a_RegQueryValueExA},
    {"advapi32.dll", "RegSetValueExA", 6, a_RegSetValueExA},
};

void hle_reg_register(void) {
    const char* id = getenv("NC_INSTALLATION_ID");
    if (!id || !*id) id = "6b1c9b9e-6a3f-4d8e-9f2a-3c5d7e0f1a2b";
    Key* app = walk(&roots[HKEY_LOCAL_MACHINE - HKEY_ROOT_FIRST],"SOFTWARE\\WOW6432Node\\Gameforge4d\\GameforgeClient\\MainApp", 1, 0);
    value_set(app, "InstallationId", REG_SZ, id, (uint32_t)strlen(id) + 1);
    hle_register(reg_table, sizeof reg_table / sizeof reg_table[0]);
}
