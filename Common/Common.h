#pragma once

#include <chrono>
#include <functional>

#define CONCAT2(p1, p2) p1 ## p2
#define CONCAT(p1, p2) CONCAT2(p1, p2)

#define STR_(val) #val
#define STR(val) STR_(val)

#define TS_COMMA(arg1, arg2, ...) arg1, arg2, ##__VA_ARGS__

#define TS_MOVABLE(class_name, type) \
	class_name(class_name &&) = type; \
	class_name &operator =(class_name &&) = type;

#define TS_COPYABLE(class_name, type) \
	class_name(const class_name &) = type; \
	class_name &operator =(const class_name &) = type;


using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace std::placeholders;
