/* JSON mínimo (DOM) só para as respostas do backend local. */
#ifndef BRIKS_JSON_H
#define BRIKS_JSON_H
#include <stdbool.h>
#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;
typedef struct jval {
    jtype_t type;
    double num;
    bool b;
    char *str;                 /* J_STR */
    struct jval *items;        /* J_ARR: itens; J_OBJ: valores */
    char **keys;               /* J_OBJ */
    int count;
} jval;

jval *json_parse(const char *text);                 /* NULL se inválido; liberar com json_free */
void json_free(jval *v);
const jval *json_get(const jval *obj, const char *key);
const char *json_str(const jval *obj, const char *key, const char *fallback);
int json_len(const jval *arr);
const jval *json_at(const jval *arr, int i);
/* Escapa uma string para dentro de um literal JSON. Devolve bytes escritos (sem o NUL). */
size_t json_escape(const char *in, char *out, size_t cap);
#endif
