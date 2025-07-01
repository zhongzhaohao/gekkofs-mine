#ifndef FSINFO_H
#define FSINFO_H
#include <ctime>
#include <limits>

#define TimeMax std::numeric_limits<time_t>::max()
#define TimeMin std::numeric_limits<time_t>::min()

struct fs_info {
    unsigned int prefix;
    unsigned int size;
    unsigned int priority;
    time_t life_start;
    time_t life_end;
};

#endif // FSINFO_H