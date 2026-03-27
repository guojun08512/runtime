
#include "enkiTs/TaskScheduler.h"
#include <iostream>
int main() 
{
    enki::TaskScheduler taskScheduler;
    taskScheduler.Initialize();
    std::cout << "Hello, enkiTS!" << std::endl;
    return 0;
}
