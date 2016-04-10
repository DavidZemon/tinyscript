cmake_minimum_required(VERSION 3.3)
find_package(PropWare 3.0 REQUIRED)

project(tinyscript)

create_simple_executable(tinyscript
    main.cpp
    tinyscript.cpp
)
