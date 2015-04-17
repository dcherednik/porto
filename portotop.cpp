#include <string>
#include <vector>
#include <list>
#include <functional>

#include "libporto.hpp"
#include "util/namespace.hpp"

extern "C" {
#include <ncurses.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
};

class TConsoleScreen {
private:
    WINDOW *Wnd;

    int Width() {
        return getmaxx(Wnd);
    }

public:
    int Height() {
        return getmaxy(Wnd);
    }

    TConsoleScreen() {
        Wnd = initscr();
        clear();
        cbreak();
        noecho();
        intrflush(stdscr, true);
        keypad(stdscr, true);
        timeout(1000);
        curs_set(0);
    }
    ~TConsoleScreen() {
        endwin();
    }
    template<class T>
    int PrintAt(T arg, int x, int y, int width, bool leftaligned) {
        return PrintAt(std::to_string(arg), x, y, width, leftaligned);
    }
    int PrintAt(std::string str, int x, int y, int width, bool leftaligned) {
        int w = x + width < Width() ? width : Width() - x;
        mvprintw(y, x, (leftaligned ? "%-*s" : "%*s"), w, str.c_str());
        return w;
    }
    void Refresh() {
        refresh();
    }
    void Clear() {
        clear();
    }
    int Getch() {
        return wgetch(Wnd);
    }
    void Save() {
        def_prog_mode();
	endwin();
    }
    void Restore() {
        reset_prog_mode();
        refresh();
    }
    int Dialog(std::string text, const std::vector<std::string> &buttons) {
        int selected = 0;

        int x0 = Width() / 2 - text.length() / 2;
        int y0 = Height() / 2 - 3;

        int w = 0;
        for (auto &b : buttons)
            w += b.length() + 1;
        int x00 = Width() / 2 - w / 2;

        while (true) {
            Clear();

            PrintAt(text, x0, y0, text.length(), false);

            int x = x00;
            int n = 0;
            for (auto &b : buttons) {
                if (n == selected)
                    attron(A_REVERSE);
                x += 1 + PrintAt(b, x, y0 + 2, b.length(), false);
                attroff(A_REVERSE);
                n++;
            }

            switch(Getch()) {
            case KEY_LEFT:
                if (--selected < 0)
                    selected = 0;
                break;
            case KEY_RIGHT:
                if ((unsigned long) ++selected > buttons.size() - 1)
                    selected = buttons.size() - 1;
                break;
            case '\n':
                return selected;
            }

            Refresh();
        }

        return -1;
    }
    void ErrorDialog(TPortoAPI *api) {
        std::string message;
        int error;

        api->GetLastError(error, message);

        if (error)
            Dialog(message, {"Ok"});
        else
            Dialog("Unknown error occured", {"Ok"});
    }
    void InfoDialog(std::vector<std::string> lines) {
        unsigned int w = 0;
        for (auto &l : lines)
            if (l.length() > w)
                w = l.length();
        int x0 = Width() / 2 - w / 2;
        int y0 = Height() / 2 - lines.size() / 2;

        while (true) {
            Clear();

            int n = 0;
            for (auto &l : lines) {
                PrintAt(l, x0, y0 + n, l.length(), false);
                n++;
            }

            switch(Getch()) {
            case 0:
            case -1:
                break;
            default:
                return;
            }

            Refresh();    
        }
    }
    void HelpDialog() {
        std::vector<std::string> help =
            {"horizontal arrows - change sorting",
             "vertical arrows - select container/scroll",
             "tab - expand subcontainers",
             "s - start/stop container",
             "p - pause/resume container",
             "k - kill container",
             "d - destroy container",
             "g - show container properties",
             "o - show container stdout",
             "e - show container stderr",
             "enter - run top in container",
             "space - pause",
             "q - quit"};
        InfoDialog(help);
    }
};

class TColumn;
class TRowTree {
    TRowTree(std::string container) : Container(container) {
        if (Container == "/")
            Level = 0;
        else
            Level = 1 + std::count(container.begin(), container.end(), '/');
    }
    TRowTree* GetParent(int level) {
        if (Parent) {
            if (Parent->GetLevel() == level)
                return Parent;
            else
                return Parent->GetParent(level);
        } else
            return nullptr;
    }
public:
    ~TRowTree() {
        for (auto &c : Children)
            delete c;
    }
    static TRowTree* ContainerTree(std::vector<std::string> &containers) {
        TRowTree *root = nullptr;
        TRowTree *curr = nullptr;
        TRowTree *prev = nullptr;
        int level = 0;

        std::sort(containers.begin(), containers.end());

        for (auto &c : containers) {
            curr = new TRowTree(c);
            level = curr->GetLevel();
            if (!root) {
                /* assume that / container is first in the list */
                if (c == "/") {
                    root = curr;
                    prev = curr;
                    continue;
                } else
                    break;
            } else if (level > prev->GetLevel())
                curr->Parent = prev;
            else if (level == prev->GetLevel())
                curr->Parent = prev->Parent;
            else /* level < prev->GetLevel() */
                curr->Parent = prev->GetParent(level - 1);

            curr->Parent->Children.push_back(curr);
            prev = curr;
        }
        return root;
    }
    std::string GetContainer() {
        return Container;
    }
    int GetLevel() {
        return Level;
    }
    void for_each(std::function<void (TRowTree&)> fn, int maxlevel) {
        if (Level <= maxlevel)
            fn(*this);
        if (Level < maxlevel)
            for (auto &c : Children)
                c->for_each(fn, maxlevel);
    }
    void Sort(TColumn &column);
    bool IsSelected() {
        return Selected;
    }
    void Select(bool select) {
        Selected = select;
    }
    int GetMaxLevel() {
        int level = Level;
        for (auto &c : Children)
            if (c->GetMaxLevel() > level)
                level = c->GetMaxLevel();
        return level;
    }
    int RowCount(int max_level) {
        int count = 1;
        if (Level < max_level)
            for (auto &c : Children)
                count += c->RowCount(max_level);
        return count;
    }
    std::string ContainerAt(int n, int max_level) {
        TRowTree *ret = this;
        int i = 0;
        for_each([&] (TRowTree &row) {
                if (i++ == n)
                    ret = &row;
            }, max_level);
        return ret->GetContainer();
    }
private:
    TRowTree* Parent = nullptr;
    std::list<TRowTree*> Children;
    std::string Container;
    int Level = 0;
    bool Selected = false;
};

typedef std::function<std::string (TRowTree&)> calc_fn;
typedef std::function<std::string (TRowTree&, std::string, unsigned long*,
                                   unsigned long*, unsigned long)> diff_fn;
typedef std::function<std::string (TRowTree&, std::string)> print_fn;

class TColumn {
public:
    TColumn(std::string title, calc_fn calc, diff_fn diff = nullptr,
            print_fn print = nullptr, bool left_aligned = false) :
        Title(title), CalcFn(calc), DiffFn(diff), PrintFn(print),
        LeftAligned(left_aligned) {
        Width = title.length();
    }
    int PrintTitle(int x, int y, TConsoleScreen &screen) {
        attron(A_BOLD);
        if (Selected)
            attron(A_UNDERLINE);
        int ret = screen.PrintAt(Title, x, y, Width, LeftAligned);
        if (Selected)
            attroff(A_UNDERLINE);
        attroff(A_BOLD);
        return ret;
    }
    int Print(TRowTree &row, int x, int y, TConsoleScreen &screen) {
        if (row.IsSelected())
            attron(A_REVERSE);
        int ret = screen.PrintAt(Cache[row.GetContainer()].to_print, x, y, Width,
                                 LeftAligned);
        if (row.IsSelected())
            attroff(A_REVERSE);
        return ret;
    }
    void Update(TRowTree* tree, unsigned long gone, int maxlevel) {
        tree->for_each([&] (TRowTree &row) {
                Cache[row.GetContainer()].value = CalcFn(row);
                if (DiffFn)
                    Cache[row.GetContainer()].value =
                        DiffFn(row,
                               Cache[row.GetContainer()].value,
                               &Cache[row.GetContainer()].prev,
                               &Cache[row.GetContainer()].pprev, gone);

                if (PrintFn)
                    Cache[row.GetContainer()].to_print =
                        PrintFn(row, Cache[row.GetContainer()].value);
                else
                    Cache[row.GetContainer()].to_print =
                        Cache[row.GetContainer()].value;

                if (Cache[row.GetContainer()].to_print.length() > Width)
                    Width = Cache[row.GetContainer()].to_print.length();
            }, maxlevel);
    }
    std::string at(TRowTree &row) {
        return Cache[row.GetContainer()].value;
    }
    void Highlight(bool enable) {
        Selected = enable;
    }
private:
    std::string Title;
    unsigned int Width;
    calc_fn CalcFn;
    diff_fn DiffFn;
    print_fn PrintFn;
    struct CacheEntry {
        std::string value;
        std::string to_print;
        unsigned long prev = 0;
        unsigned long pprev = 0;
    };
    std::map<std::string, struct CacheEntry> Cache;
    bool Selected = false;
    bool LeftAligned = false;
};

void TRowTree::Sort(TColumn &column) {
    Children.sort([&] (TRowTree *row1, TRowTree *row2) {
            std::string str1 = column.at(*row1);
            std::string str2 = column.at(*row2);

            try {
                return stod(str1) > stod(str2);
            } catch (...) {
                return str1 < str2;
            }
        });
    for (auto &c : Children)
        c->Sort(column);
}

class TTable {
    void PrintTitle(int y, TConsoleScreen &screen) {
        int x = 0;
        for (auto &c : Columns)
            x += 1 + c.PrintTitle(x, y, screen);
    }
public:
    void Print(TConsoleScreen &screen) {
        MaxRows = RowTree->RowCount(MaxLevel);
        DisplayRows = std::min(screen.Height() - 1, MaxRows);
        ChangeSelection(0, 0);

        screen.Clear();
        PrintTitle(0, screen);
        int x = 0;
        int y = 0;
        RowTree->for_each([&] (TRowTree &row) {
                if (y >= FirstRow && y < MaxRows) {
                    if (y == FirstRow + SelectedRow)
                        row.Select(true);
                    for (auto &c : Columns)
                        x += 1 + c.Print(row, x, y + 1 - FirstRow, screen);
                    row.Select(false);
                    x = 0;
                }
                y++;
            }, MaxLevel);
        screen.Refresh();
    }
    void AddColumn(const TColumn &c) {
        Columns.push_back(c);
    }
    void Update(TConsoleScreen &screen, TPortoAPI *api) {
        LastUpdate = Now;
        clock_gettime(CLOCK_MONOTONIC, &Now);
        unsigned long gone = 1000 * (Now.tv_sec - LastUpdate.tv_sec) +
            (Now.tv_nsec - LastUpdate.tv_nsec) / 1000000;

        if (gone < 300)
            return;

        std::vector<std::string> containers;
        int ret = api->List(containers);
        if (ret)
            exit(EXIT_FAILURE);

        if (RowTree)
            delete RowTree;

        RowTree = TRowTree::ContainerTree(containers);
        if (RowTree) {
            MaxMaxLevel = RowTree->GetMaxLevel();

            for (auto &column : Columns)
                column.Update(RowTree, gone, MaxLevel);

            RowTree->Sort(Columns[SelectedColumn]);
        }
    }
    void ChangeSelection(int x, int y) {
        SelectedRow += y;
        if (SelectedRow < 0) {
            SelectedRow = 0;
            FirstRow += y;
            if (FirstRow < 0)
                FirstRow = 0;
        }
        if (SelectedRow > DisplayRows - 1) {
            SelectedRow = DisplayRows - 1;
            FirstRow += y;
            if (FirstRow > MaxRows - DisplayRows)
                FirstRow = MaxRows - DisplayRows;
        }

        Columns[SelectedColumn].Highlight(false);
        SelectedColumn += x;
        if (SelectedColumn < 0)
            SelectedColumn = Columns.size() - 1;
        else
            SelectedColumn %= Columns.size();
        Columns[SelectedColumn].Highlight(true);
    }
    void Expand() {
        if (++MaxLevel > MaxMaxLevel)
            MaxLevel = 0;
    }
    int StartStop(TPortoAPI *api) {
        std::string state;
        api->GetData(SelectedContainer(), "state", state);
        if (state == "running" || state == "dead")
            return api->Stop(SelectedContainer());
        else if (state == "stopped")
            return api->Start(SelectedContainer());
        else
            return -1;
    }
    int PauseResume(TPortoAPI *api) {
        std::string state;
        api->GetData(SelectedContainer(), "state", state);
        if (state == "paused")
            return api->Resume(SelectedContainer());
        else if (state == "running")
            return api->Pause(SelectedContainer());
        else
            return -1;
    }
    int Kill(TPortoAPI *api, int signal) {
        std::string state;
        api->GetData(SelectedContainer(), "state", state);
        if (state == "running")
            return api->Kill(SelectedContainer(), signal);
        else
            return -1;
    }
    int Destroy(TPortoAPI *api) {
        return api->Destroy(SelectedContainer());
    }
    int RunTop(TPortoAPI *api) {
        if (SelectedContainer() != "/") {
            std::string pidStr;
            int ret = api->GetData(SelectedContainer(), "root_pid", pidStr);
            if (ret)
                return -1;

            int pid;
            TError error = StringToInt(pidStr, pid);
            if (error)
                return -1;

            TNamespaceSnapshot guest_ns;
            error = guest_ns.Create(pid);
            if (error)
                return -1;

            TNamespaceSnapshot my_ns;
            error = my_ns.Create(getpid());
            if (error)
                return -1;

            error = guest_ns.Attach();
            if (error)
                return -1;

            system("top");

            my_ns.Attach();
        } else
            system("top");

        return 0;
    }
    void LessPortoctl(std::string container, std::string cmd) {
        std::string s(program_invocation_name);
        s += " get " + container + " " + cmd + " | less";
        system(s.c_str());
    }
    std::string SelectedContainer() {
        return RowTree->ContainerAt(FirstRow + SelectedRow, MaxLevel);
    }
private:
    std::vector<TColumn> Columns;
    TRowTree* RowTree = nullptr;
    int SelectedRow = 0;
    int SelectedColumn = 0;
    int FirstRow = 0;
    int MaxRows = 0;
    int DisplayRows = 0;
    int MaxLevel = 1;
    int MaxMaxLevel = 1;
    struct timespec LastUpdate = {0};
    struct timespec Now = {0};;
};

static print_fn nice_number(int base) {
    return [=] (TRowTree &row, std::string raw) {
        try {
            char buf[20];
            char s = 0;

            double v = stod(raw);
            if (v > base * base * base) {
                v /= base * base * base;
                s = 'G';
            } else if (v > base * base) {
                v /= base * base;
                s = 'M';
            } else if (v > base) {
                v /= base;
                s = 'k';
            }

            snprintf(buf, sizeof(buf), "%.1lf%c", v, s);
            return std::string(buf);
        } catch (...) {
            return std::string();
        }
    };
}

static print_fn nice_seconds(double multiplier = 1) {
    return [=] (TRowTree &row, std::string raw) {
        try {
            char buf[40];

            double seconds = stod(raw) / multiplier;
            double minutes = floor(seconds / 60);
            seconds -= minutes * 60;

            snprintf(buf, sizeof(buf), "%4.lf:%2.2lf", minutes, seconds);
            return std::string(buf);
        } catch (...) {
            return std::string();
        }
    };
}

static print_fn nice_percents() {
    return [] (TRowTree &row, std::string raw) {
        try {
            char buf[20];
            snprintf(buf, sizeof(buf), "%.1lf%%", 100 * stod(raw));
            return std::string(buf);
        } catch (...) {
            return std::string();
        }
        return raw;
    };
}

static calc_fn container_data(TPortoAPI *api, std::string data) {
    return [=] (TRowTree &row) {
        std::string curr;
        api->GetData(row.GetContainer(), data, curr);
        return curr;
    };
}

static calc_fn map_summ(TPortoAPI *api, std::string data) {
    return [=] (TRowTree &row) {
        std::string value;
        api->GetData(row.GetContainer(), data, value);
        std::vector<std::string> values;
        unsigned long start_v = 0;
        for (unsigned long off = 0; off < value.length(); off++) {
            if (value[off] == ':') {
                start_v = off + 2; // key: value
            } else if (value[off] == ';') {
                values.push_back(value.substr(start_v, off - start_v));
            }
        }
        values.push_back(value.substr(start_v));

        unsigned long ret = 0;
        try {
            for (auto &s : values)
                ret += stoull(s);
        } catch (...) {
            ret = 0;
        }

        return std::to_string(ret);
    };
}


static calc_fn container_property(TPortoAPI *api, std::string property) {
    return [=] (TRowTree &row) {
        std::string curr;
        api->GetProperty(row.GetContainer(), property, curr);
        return curr;
    };
}

static diff_fn diff_percents_of_root(TPortoAPI *api, std::string data) {
    return [=] (TRowTree &row, std::string value, unsigned long *prev,
                unsigned long *pprev, unsigned long gone) {
        std::string pcurr;
        api->GetData("/", data, pcurr);
                              
        try {
            unsigned long c = stoull(value);
            unsigned long pc = stoull(pcurr);

            if (pc == *pprev)
                return std::string("0");

            std::string str = std::to_string(1.0d * (c - *prev) /
                                             (pc - *pprev));
            *prev = c;
            *pprev = pc;

            return str;
        } catch (...) {
            return std::string();
        }
    };
}

static diff_fn diff(TPortoAPI *api = nullptr) {
    return [=] (TRowTree &row, std::string value, unsigned long *prev,
                unsigned long *pprev, unsigned long gone) {
        try {
            unsigned long c = stoull(value);

            std::string str = std::to_string((c - *prev) / gone);
            *prev = c;

            return str;
        } catch (...) {
            return std::string();
        }
    };
}

int portotop(TPortoAPI *api) {
    TTable top;

    /* Common */
    top.AddColumn(TColumn("container",
                          [] (TRowTree &row) {
                              return row.GetContainer();
                          },
                          nullptr,
                          [] (TRowTree &row, std::string curr) {
                              int level = row.GetLevel();
                              if (level > 1)
                                  curr = std::string("\\_ ") +
                                      curr.substr(1 + curr.rfind('/'));
                              return std::string(level, ' ') + curr;
                          },
                          true));
    top.AddColumn(TColumn("state", container_data(api, "state")));
    top.AddColumn(TColumn("time", container_data(api, "time"),
                          nullptr, nice_seconds(1)));

    /* CPU */
    top.AddColumn(TColumn("policy", container_property(api, "cpu_policy")));
    top.AddColumn(TColumn("cpu%", container_data(api, "cpu_usage"),
                          diff_percents_of_root(api, "cpu_usage"),
                          nice_percents()));
    top.AddColumn(TColumn("cpu", container_data(api, "cpu_usage"),
                          nullptr, nice_seconds(1E9)));

    /* Memory */
    top.AddColumn(TColumn("memory", container_data(api, "memory_usage"),
                          nullptr, nice_number(1024)));
    top.AddColumn(TColumn("limit", container_property(api, "memory_limit"),
                          nullptr, nice_number(1024)));
    top.AddColumn(TColumn("guarantee",
                          container_property(api, "memory_guarantee"),
                          nullptr, nice_number(1024)));

    /* I/O */
    top.AddColumn(TColumn("maj/s", container_data(api, "major_faults"),
                          diff(), nice_number(1000)));
    top.AddColumn(TColumn("read b/s", map_summ(api, "io_read"),
                          diff(),  nice_number(1000)));
    top.AddColumn(TColumn("write b/s", map_summ(api, "io_write"),
                          diff(), nice_number(1000)));

    /* Network */
    top.AddColumn(TColumn("net b/s",
                          map_summ(api, "net_bytes"),
                          diff(), nice_number(1024)));

    /* Main loop */
    TConsoleScreen screen;
    bool paused = false;
    while (true) {
        if (!paused)
            top.Update(screen, api);

        top.Print(screen);

        switch (screen.Getch()) {
        case 'q':
        case 'Q':
            return EXIT_SUCCESS;
            break;
        case KEY_UP:
            top.ChangeSelection(0, -1);
            break;
        case KEY_PPAGE:
            top.ChangeSelection(0, -10);
            break;
        case KEY_DOWN:
            top.ChangeSelection(0, 1);
            break;
        case KEY_NPAGE:
            top.ChangeSelection(0, 10);
            break;
        case KEY_LEFT:
            top.ChangeSelection(-1, 0);
            break;
        case KEY_RIGHT:
            top.ChangeSelection(1, 0);
            break;
        case '\t':
            top.Expand();
            break;
        case ' ':
            paused = !paused;
            break;
        case 's':
        case 'S':
            if (screen.Dialog("Start/stop container " + top.SelectedContainer(),
                              {"No", "Yes"}) == 1)
                if (top.StartStop(api))
                    screen.ErrorDialog(api);
            break;
        case 'p':
        case 'P':
            if (screen.Dialog("Pause/resume container " + top.SelectedContainer(),
                              {"No", "Yes"}) == 1)
                if (top.PauseResume(api))
                    screen.ErrorDialog(api);
            break;
        case 'k':
        case 'K':
        {
            int signal = -1;
            switch (screen.Dialog("Kill container " + top.SelectedContainer(),
                                  {"Cancel", "SIGTERM", "SIGINT", "SIGKILL", "SIGHUP"})) {
            case 1:
                signal = SIGTERM;
                break;
            case 2:
                signal = SIGINT;
                break;
            case 3:
                signal = SIGKILL;
                break;
            case 4:
                signal = SIGHUP;
                break;
            }
            if (signal > 0)
                if (top.Kill(api, signal))
                    screen.ErrorDialog(api);
            break;
        }
        case 'd':
        case 'D':
            if (screen.Dialog("Destroy container " + top.SelectedContainer(),
                              {"No", "Yes"}) == 1)
                if (top.Destroy(api))
                    screen.ErrorDialog(api);
            break;
        case '\n':
            screen.Save();
            if (top.RunTop(api)) {
                screen.Restore();
                screen.ErrorDialog(api);
            } else
                screen.Restore();
            break;
        case 'g':
        case 'G':
            screen.Save();
            top.LessPortoctl(top.SelectedContainer(), "");
            screen.Restore();
            break;
        case 'o':
        case 'O':
            screen.Save();
            top.LessPortoctl(top.SelectedContainer(), "stdout");
            screen.Restore();
            break;
        case 'e':
        case 'E':
            screen.Save();
            top.LessPortoctl(top.SelectedContainer(), "stderr");
            screen.Restore();
            break;
        case 0:
        case -1:
            break;
        case '?':
        case 'h':
        default:
            screen.HelpDialog();
            break;
        }
    }

    return EXIT_SUCCESS;
}