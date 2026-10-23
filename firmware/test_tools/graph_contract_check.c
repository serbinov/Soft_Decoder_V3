#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "sound_graph.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s payload.json\n", argv[0]);
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    if (file == NULL) {
        perror("open");
        return 2;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 2;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 2;
    }
    char *json = malloc((size_t)size + 1U);
    if (json == NULL) {
        fclose(file);
        return 2;
    }
    size_t read = fread(json, 1, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size) {
        free(json);
        return 2;
    }
    json[size] = '\0';

    sg_graph_t *graph = calloc(1, sizeof(*graph));
    if (graph == NULL) {
        free(json);
        return 2;
    }
    sg_diagnostic_t diagnostic = {0};
    esp_err_t parse_err = sg_parse(json, (size_t)size, graph, &diagnostic);
    esp_err_t validate_err = parse_err;
    if (parse_err == ESP_OK) {
        validate_err = sg_validate(graph, &diagnostic);
    }
    printf("parse=%d validate=%d states=%u transitions=%u effects=%u id=%s field=%s message=%s\n",
           (int)parse_err, (int)validate_err,
           graph->state_count, graph->transition_count, graph->effect_count,
           diagnostic.id, diagnostic.field, diagnostic.message);
    free(graph);
    free(json);
    return validate_err == ESP_OK ? 0 : 1;
}
