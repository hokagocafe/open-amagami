#include "dialog.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define AMG_ISATTY _isatty
#define AMG_FILENO _fileno
#else
#include <unistd.h>
#define AMG_ISATTY isatty
#define AMG_FILENO fileno
#endif

namespace amg::dialog {

namespace {

const char* button_names(Buttons b) {
    return b == Buttons::Ok ? "OK" : b == Buttons::YesNo ? "Yes/No" : "Yes/No/Cancel";
}

Reply parse_reply(const std::string& word, Buttons buttons) {
    std::string w;
    for (char c : word) w += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (buttons == Buttons::Ok) return Reply::Ok;
    if (w == "yes" || w == "y") return Reply::Yes;
    if (w == "no" || w == "n") return Reply::No;
    if (w == "cancel" && buttons == Buttons::YesNoCancel) return Reply::Cancel;
    return Reply::Yes;  // the first button
}

const char* reply_name(Reply r) {
    return r == Reply::Ok ? "ok" : r == Reply::Yes ? "yes" : r == Reply::No ? "no" : "cancel";
}

std::string flat(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s;
}

class Console : public Service {
public:
    explicit Console(int forced) : forced_(forced) {}
    Reply show(const std::string& message, Buttons buttons, Icon icon, bool default_no) override {
        std::FILE* out = icon == Icon::Warning || icon == Icon::Error ? stderr : stdout;
        std::fprintf(out, "%s\n", message.c_str());
        std::fflush(out);
        if (buttons == Buttons::Ok) return Reply::Ok;
        if (forced_ >= 0) {
            std::fprintf(out, "[%s]\n", forced_ ? "yes" : "no");
            return forced_ ? Reply::Yes : Reply::No;
        }
        if (!AMG_ISATTY(AMG_FILENO(stdin))) {  // nobody to ask: the safe answer
            std::fprintf(out, "[%s: no terminal to ask on; use --yes or --no]\n", buttons == Buttons::YesNoCancel ? "cancel" : "no");
            return buttons == Buttons::YesNoCancel ? Reply::Cancel : Reply::No;
        }
        for (;;) {
            std::fprintf(out, buttons == Buttons::YesNoCancel ? "[y]es / [n]o / [c]ancel%s: " : "[y]es / [n]o%s: ", default_no ? " (default: no)" : "");
            std::fflush(out);
            std::string line;
            if (!std::getline(std::cin, line)) return buttons == Buttons::YesNoCancel ? Reply::Cancel : Reply::No;
            if (line.empty() && default_no) return Reply::No;
            const char c = line.empty() ? ' ' : char(line[0] | 0x20);
            if (c == 'y') return Reply::Yes;
            if (c == 'n') return Reply::No;
            if (c == 'c' && buttons == Buttons::YesNoCancel) return Reply::Cancel;
        }
    }

private:
    int forced_;
};

class Scripted : public Service {
public:
    Scripted(const std::string& answers, std::string log) : log_(std::move(log)) {
        std::stringstream in(answers);
        for (std::string item; std::getline(in, item, ',');) answers_.push_back(item);
    }
    Reply show(const std::string& message, Buttons buttons, Icon, bool) override {
        const Reply reply = next_ < answers_.size() ? parse_reply(answers_[next_], buttons) : (buttons == Buttons::Ok ? Reply::Ok : Reply::Yes);
        ++next_;
        if (!log_.empty()) {
            std::ofstream out(std::filesystem::u8path(log_), std::ios::binary | std::ios::app);
            out << flat(message) << " | " << button_names(buttons) << " | " << reply_name(reply) << "\n";
        }
        return reply;
    }

private:
    std::vector<std::string> answers_;
    size_t next_ = 0;
    std::string log_;
};

Service* g_service = nullptr;

}  // namespace

void set(Service* service) { g_service = service; }

Service& current() {
    static Console fallback(-1);  // a program that never chose one still answers
    return g_service ? *g_service : fallback;
}

std::unique_ptr<Service> make_console(int forced) { return std::make_unique<Console>(forced); }

std::unique_ptr<Service> make_scripted(const std::string& answers, const std::string& log_path) { return std::make_unique<Scripted>(answers, log_path); }

}  // namespace amg::dialog
