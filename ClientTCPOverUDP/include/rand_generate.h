#ifndef RAND_GENERATE_H_INCLUDED
#define RAND_GENERATE_H_INCLUDED

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdint.h>

using namespace std;

unsigned int hash3(unsigned int h1, unsigned int h2, unsigned int h3);

//use this first function to seed the random number generator,
//call this before any of the other functions
void initrand();

//generates a psuedo-random integer between 0 and 32767
int randint();

//generates a psuedo-random integer between 0 and max
/// 10.0*rand()/(RAND_MAX+1.0)) выдает, соответственно, от 0 до 10 (не включая 10)
int randint( int max );

//generates a psuedo-random integer between min and max
int randint(int min, int max);

//generates a psuedo-random float between 0.0 and 0.999...
float randfloat();

//generates a psuedo-random float between 0.0 and max
float randfloat(float max);

//generates a psuedo-random float between min and max
float randfloat(float min, float max);

//generates a psuedo-random double between 0.0 and 0.999...
double randdouble();

//generates a psuedo-random double between 0.0 and max
double randdouble(double max);

//generates a psuedo-random double between min and max
double randdouble(double min, double max);

// random generator function:
int myrandom (int i);

/// в микросекундах  1 / 1 000 000
uint64_t get_timestamp();

/// в милисекундах   1 / 1000
uint64_t get_time_in_ms();

#endif // RAND_GENERATE_H_INCLUDED
