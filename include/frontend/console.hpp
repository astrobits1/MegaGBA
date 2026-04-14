#ifndef FRONT_CONSOLE_H
#define FRONT_CONSOLE_H

#include <iostream>

class Context;

class Console {
    Context* ctx;
    std::string outputBuffer;
 
    void output(std::string out);
    std::string error(std::string err);
public:
    Console(Context* ctx) : ctx(ctx) {};
    std::string runCommand(std::string inp);

    /* Functions */
};

#endif
