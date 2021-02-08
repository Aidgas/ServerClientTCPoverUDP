#ifndef UTIL_STRING_H_INCLUDED
#define UTIL_STRING_H_INCLUDED

#include <iostream>
#include <map>
#include <string>
#include <sstream>
#include <vector>
#include <stdio.h>
#include <stdarg.h>

#include <algorithm>
#include <functional>
#include <cctype>
#include <locale>

using namespace std;

std::string string_format(const std::string &fmt, ...);
string string_replace( string src, string const& target, string const& repl);

vector<string> string_split(const string &s, char delim);

static inline std::string &ltrim(std::string &s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
        return s;
}

// trim from end
static inline std::string &rtrim(std::string &s) {
        s.erase(std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
        return s;
}

// trim from both ends
static inline std::string &trim(std::string &s) {
        return ltrim(rtrim(s));
}

void string_replace_all(std::string& str, const std::string& from, const std::string& to);

string join(const vector<string>& vec, string delim);

std::string strtruncate(std::string str, size_t width, bool show_ellipsis=true);

#endif // UTIL_STRING_H_INCLUDED
