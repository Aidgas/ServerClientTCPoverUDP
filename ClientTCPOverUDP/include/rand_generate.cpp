#include "rand_generate.h"

unsigned int hash3(unsigned int h1, unsigned int h2, unsigned int h3)
{
    return ( ((h1 * 2654435789U) + h2) * 2654435789U) + h3;
}

//use this first function to seed the random number generator,
//call this before any of the other functions
void initrand()
{
    //srand((unsigned)(time(0)));

    timeval time;
    gettimeofday(&time, NULL);
    srand(hash3(time.tv_sec, time.tv_usec, getpid()));
}

//generates a psuedo-random integer between 0 and 32767
int randint()
{
    return rand();
}

//generates a psuedo-random integer between 0 and max
/// 10.0*rand()/(RAND_MAX+1.0)) выдает, соответственно, от 0 до 10 (не включая 10)
int randint( int max )
{
    return int( max*rand()/(RAND_MAX+1.0) );
}

//generates a psuedo-random integer between min and max
int randint(int min, int max)
{
    if (min > max)
    {
        return max+int( (min-max+1) * rand() / (RAND_MAX+1.0) );
    }
    else
    {
        return min+int( (max-min+1) * rand() / (RAND_MAX+1.0) );
    }
}

//generates a psuedo-random float between 0.0 and 0.999...
float randfloat()
{
    return rand()/(float(RAND_MAX)+1);
}

//generates a psuedo-random float between 0.0 and max
float randfloat(float max)
{
    return randfloat()*max;
}

//generates a psuedo-random float between min and max
float randfloat(float min, float max)
{
    if (min>max)
    {
        return randfloat()*(min-max)+max;
    }
    else
    {
        return randfloat()*(max-min)+min;
    }
}

//generates a psuedo-random double between 0.0 and 0.999...
double randdouble()
{
    return rand()/(double(RAND_MAX)+1);
}

//generates a psuedo-random double between 0.0 and max
double randdouble(double max)
{
    return randdouble()*max;
}

//generates a psuedo-random double between min and max
double randdouble(double min, double max)
{
    if (min>max)
    {
        return randdouble()*(min-max)+max;
    }
    else
    {
        return randdouble()*(max-min)+min;
    }
}

int myrandom (int i) { return rand()%i; }

/// в микросекундах
uint64_t get_timestamp()
{
    struct timeval currentTime;
    gettimeofday(&currentTime, NULL);
    return currentTime.tv_sec * (uint64_t)1000000 + currentTime.tv_usec;
}

/// в милисекундах
uint64_t get_time_in_ms()
{
    struct timeval tp;
    gettimeofday(&tp, NULL);
    return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}
