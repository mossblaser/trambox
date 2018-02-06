#ifndef METROLINK_H
#define METROLINK_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Call exactly once on startup before using any other functions.
 */
void metrolink_init(void);

/**
 * Specify the journey desired by the user.
 */
void metrolink_set_journey(const char *start, const char *target);

/**
 * If a tram with the destination name supplied shows up at the 'start' station
 * given to metrolink_set_journey, will it stop at the 'target' given?
 */
bool metrolink_is_destination_valid(const char *target);

#endif
