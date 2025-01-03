#pragma once
#include <sstream>
#include <iomanip>
#include <thread>

inline std::string get_thread_id() {
    std::stringstream ss;
    ss << "[Thread-" << std::setw(5) << std::this_thread::get_id() << "] ";
    return ss.str();
} 