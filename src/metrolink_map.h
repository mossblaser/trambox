#ifndef METROLINK_MAP_H
#define METROLINK_MAP_H

#include <stddef.h>

/**
 * The complete list of Metrolink station names.
 */
extern const char *METROLINK_STATIONS[];

/**
 * The number of Metrolink stations.
 */
extern size_t NUM_METROLINK_STATIONS;

typedef struct {
	const char *a;
	const char *b;
} metrolink_name_pair_t;

/**
 * Lists (for one direction only) connections between stations.
 */
extern metrolink_name_pair_t METROLINK_LINKS[];

/**
 * Number of entries in METROLINK_LINKS.
 */
extern size_t NUM_METROLINK_LINKS;

#endif
