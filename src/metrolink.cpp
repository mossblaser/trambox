#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "metrolink.h"
#include "metrolink_map.h"

/**
 * A node of a linked-list of station indices.
 */
typedef struct link {
	size_t station_index;
	struct link *next;
} link_t;

/**
 * An adjacency-list style graph consisting of a linked-list of (undirected)
 * neighbour edges, one per station. Initialise this structure with
 * metrolink_init.
 */
static link_t **network;

/**
 * True if a given metrolink station is a valid destination.
 */
static bool *valid_destinations;

/**
 * True if a given metrolink station has been visted by the graph traversal
 * logic.
 */
static bool *visited;

/**
 * Get the index of the station with the given name (must match exactly).
 */
static int get_exact_station_index(const char *needle) {
	for (size_t i = 0; i < NUM_METROLINK_STATIONS; i++) {
		if (strcmp(METROLINK_STATIONS[i], needle) == 0) {
			return i;
		}
	}
	return -1;
}

bool metrolink_station_names_equal(const char *a, const char *b) {
	bool match = true;
	while (*a && *b) {
		// Skip whitespace and punctuation
		while (!(isalnum(*a) || *a == ' ') && *a) {
			a++;
		}
		while (!(isalnum(*b) || *b == ' ') && *b) {
			b++;
		}
		
		// Skip 'via'
		if (strncmp(a, "via ", 4) == 0) {
			a += strlen(a);
		}
		if (strncmp(b, "via ", 4) == 0) {
			b += strlen(b);
		}
		
		// Case-insensitive search
		if (tolower(*a) != tolower(*b)) {
			match = false;
			break;
		}
		a++;
		b++;
	}
	return match && *a == *b;
}

/**
 * Get the index of the station with the given name (ignoring punctuation and
 * case and 'via' clauses).
 */
static int get_station_index(const char *needle) {
	for (size_t i = 0; i < NUM_METROLINK_STATIONS; i++) {
		const char *a = METROLINK_STATIONS[i];
		const char *b = needle;
		
		if (metrolink_station_names_equal(a, b)) {
			return i;
		}
	}
	return -1;
}

/**
 * Initialise the 'network' graph. Call exactly once on startup.
 */
void metrolink_init(void) {
	network = new link_t*[NUM_METROLINK_STATIONS];
	visited = new bool[NUM_METROLINK_STATIONS];
	valid_destinations = new bool[NUM_METROLINK_STATIONS];
	
	for (size_t i = 0; i < NUM_METROLINK_STATIONS; i++) {
		network[i] = NULL;
	}
	
	for (size_t i = 0; i < NUM_METROLINK_LINKS; i++) {
		link_t *link_a = new link_t;
		link_t *link_b = new link_t;
		
		link_a->station_index = get_exact_station_index(METROLINK_LINKS[i].b);
		link_b->station_index = get_exact_station_index(METROLINK_LINKS[i].a);
		
		link_a->next = network[link_b->station_index];
		network[link_b->station_index] = link_a;
		
		link_b->next = network[link_a->station_index];
		network[link_a->station_index] = link_b;
	}
}

/**
 * Visit a station and all unvisited stations reachable from it. If the target
 * station is reached, mark that station and all reached beyond it as 'valid'.
 *
 * @param index The index of the station to visit
 * @param target The index of the target station
 * @param already_reached Has an earlier traversal step already reached the
 *        target station?
 */
static void visit(size_t index, size_t target, bool already_reached) {
	bool valid = already_reached || (index == target);
	
	visited[index] = true;
	valid_destinations[index] |= valid;
	
	link_t *neighbours_list = network[index];
	while (neighbours_list) {
		size_t neighbour_index = neighbours_list->station_index;
		neighbours_list = neighbours_list->next;
		
		if (!visited[neighbour_index]) {
			visit(neighbour_index, target, valid);
		}
	}
	
	visited[index] = false;
}


void metrolink_set_journey(const char *start, const char *target) {
	for (size_t i = 0; i < NUM_METROLINK_STATIONS; i++) {
		visited[i] = false;
		valid_destinations[i] = false;
	}
	
	int start_index = get_station_index(start);
	int target_index = get_station_index(target);
	
	// Special case: endpoint 
	if (start_index < 0 || target_index < 0) {
		return;
	}
	
	if (start_index == target_index) {
		return;
	}
	
	visit(start_index, target_index, false);
}

bool metrolink_is_destination_valid(const char *target) {
	int index = get_station_index(target);
	if (index < 0) {
		return false;
	}

	return valid_destinations[index];
}
