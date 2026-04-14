#ifndef FRONT_CONSOLE_H
#define FRONT_CONSOLE_H

#include <iostream>

class Context;

class Console {
    Context* ctx;

public:
    Console(Context* ctx) : ctx(ctx) {};
    std::string run(std::string cmd);
};

#endif
