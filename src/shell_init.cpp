// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include <regex>

#include "mamba/shell_init.hpp"
#include "mamba/output.hpp"
#include "mamba/util.hpp"
#include "mamba/activation.hpp"
#include "mamba/environment.hpp"

#include "thirdparty/termcolor.hpp"

#include <reproc++/run.hpp>

#ifndef _WIN32
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <inttypes.h>
#if defined(__linux__)
#include <linux/limits.h>
#else
#include <limits.h>
#endif
#else
#include <windows.h>
#include <intrin.h>
#include "thirdparty/WinReg.hpp"
#endif

namespace mamba
{
    namespace
    {
        // Here we are embedding the shell scripts
        constexpr const char mamba_sh[] =
#include "../data/mamba.sh"
            ;
        constexpr const char mamba_bat[] =
#include "../data/micromamba.bat"
            ;
        constexpr const char _mamba_activate_bat[] =
#include "../data/_mamba_activate.bat"
            ;
        constexpr const char mamba_hook_bat[] =
#include "../data/mamba_hook.bat"
            ;
        constexpr const char mamba_hook_ps1[] =
#include "../data/mamba_hook.ps1"
            ;
        constexpr const char mamba_psm1[] =
#include "../data/Mamba.psm1"
            ;
        constexpr const char mamba_xsh[] =
#include "../data/mamba.xsh"
            ;

        std::regex CONDA_INITIALIZE_RE_BLOCK("# >>> mamba initialize >>>(?:\n|\r\n)?"
                                             "([\\s\\S]*?)"
                                             "# <<< mamba initialize <<<(?:\n|\r\n)?");

        std::regex CONDA_INITIALIZE_PS_RE_BLOCK("#region mamba initialize(?:\n|\r\n)?"
                                                "([\\s\\S]*?)"
                                                "#endregion(?:\n|\r\n)?");
    }

    std::string guess_shell()
    {
        auto shell = env::get("shell");
        // auto penv = [](const char* x) {
        //     std::cout << x << ": " << (std::getenv(x)  ? std::getenv(x) : "") << std::endl;
        // };
        // penv("BASH_VERSION");
        // penv("XONSH_VERSION") ;
        // penv("CMDEXTVERSION");
        // penv("PSModulePath");

        auto bash_version = env::get("BASH_VERSION");
        if (!bash_version.empty() || shell == "bash")
        {
            return "bash";
        }
        auto zsh_version = env::get("ZSH_VERSION");
        if (!zsh_version.empty() || shell == "zsh")
        {
            return "zsh";
        }
        auto xonsh_version = env::get("XONSH_VERSION");
        if (!xonsh_version.empty())
        {
            return "xonsh";
        }
        auto cmd_exe_version = env::get("CMDEXTVERSION");
        if (!cmd_exe_version.empty())
        {
            return "cmd.exe";
        }
        auto psmodule_path = env::get("PSModulePath");
        if (!psmodule_path.empty())
        {
            return "powershell";
        }
        return "";
    }

#ifdef _WIN32
    void init_cmd_exe_registry(const std::wstring& reg_path,
                               const fs::path& conda_prefix,
                               bool reverse)
    {
        winreg::RegKey key{ HKEY_CURRENT_USER, reg_path };
        std::wstring prev_value = key.GetStringValue(L"AutoRun");
        // std::wstring hook_path = '"%s"' % join(conda_prefix, 'condabin', 'conda_hook.bat')
        std::wstring hook_string = std::wstring(L"\"")
                                   + (conda_prefix / "condabin" / "mamba_hook.bat").wstring()
                                   + std::wstring(L"\"");

        if (reverse)
        {
            // Not implemented yet
        }
        else
        {
            std::wstring replace_str(L"__CONDA_REPLACE_ME_123__");
            std::wregex hook_regex(L"(\"[^\"]*?mamba[-_]hook\\.bat\")",
                                   std::regex_constants::icase);
            prev_value = std::regex_replace(
                prev_value, hook_regex, replace_str, std::regex_constants::format_first_only);

            replace_all(prev_value, replace_str, hook_string);
            std::wstring new_value = prev_value;

            if (new_value.find(hook_string) == new_value.npos)
            {
                if (!new_value.empty())
                {
                    new_value += L" & " + hook_string;
                }
                else
                {
                    new_value = hook_string;
                }
            }
            if (new_value != prev_value)
            {
                std::cout << "Adding to cmd.exe AUTORUN: " << termcolor::green;
                std::wcout << new_value;
                std::cout << termcolor::reset << std::endl;
                key.SetStringValue(L"AutoRun", new_value);
            }
        }
    }
#endif

    // Heavily inspired by https://github.com/gpakosz/whereami/
    // check their source to add support for other OS
    fs::path get_self_exe_path()
    {
#ifdef _WIN32
        DWORD size;
        std::wstring buffer(MAX_PATH, '\0');
        size = GetModuleFileNameW(NULL, (wchar_t*) buffer.c_str(), (DWORD) buffer.size());
        if (size == 0)
        {
            throw std::runtime_error("Could find location of the micromamba executable!");
        }
        else if (size == buffer.size())
        {
            DWORD new_size = size;
            do
            {
                new_size *= 2;
                buffer.reserve(new_size);
                size = GetModuleFileNameW(NULL, (wchar_t*) buffer.c_str(), (DWORD) buffer.size());
            } while (new_size == size);
        }
        return fs::absolute(buffer);
#elif defined(__APPLE__)
        uint32_t size = PATH_MAX;
        std::vector<char> buffer(size);
        if (_NSGetExecutablePath(buffer.data(), &size) == -1)
        {
            buffer.reserve(size);
            if (!_NSGetExecutablePath(buffer.data(), &size))
            {
                throw std::runtime_error("Couldn't find location the micromamba executable!");
            }
        }
        return fs::absolute(buffer.data());
#else
#if defined(__sun)
        return fs::read_symlink("/proc/self/path/a.out");
#else
        return fs::read_symlink("/proc/self/exe");
#endif
#endif
    }

    // this function calls cygpath to convert win path to unix
    std::string native_path_to_unix(const std::string& path, bool is_a_path_env)
    {
        fs::path bash = env::which("bash");
        std::string command = bash.empty() ? "cygpath" : bash.parent_path() / "cygpath";
        std::string out, err;
        try
        {
            std::vector<std::string> args{ command, path };
            if (is_a_path_env)
                args.push_back("--path");
            auto [status, ec] = reproc::run(
                args, reproc::options{}, reproc::sink::string(out), reproc::sink::string(err));
            if (ec)
            {
                throw std::runtime_error(ec.message());
            }
            return std::string(strip(out));
        }
        catch (...)
        {
            std::cout
                << termcolor::red
                << "ERROR: Could not find bash, or use cygpath to convert Windows path to Unix."
                << termcolor::reset << std::endl;
            exit(1);
        }
    }


    std::string rcfile_content(const fs::path& env_prefix,
                               const std::string& shell,
                               const fs::path& mamba_exe)
    {
        std::stringstream content;

        // todo use get bin dir here!
#ifdef _WIN32
        std::string cyg_mamba_exe = native_path_to_unix(mamba_exe.string());
        std::string cyg_env_prefix = native_path_to_unix(env_prefix.string());
        content << "# >>> mamba initialize >>>\n";
        content << "# !! Contents within this block are managed by 'mamba init' !!\n";
        content << "export MAMBA_EXE=" << std::quoted(cyg_mamba_exe, '\'') << ";\n";
        content << "export MAMBA_ROOT_PREFIX=" << std::quoted(cyg_env_prefix, '\'') << ";\n";
        content << "eval \"$(" << std::quoted(cyg_mamba_exe, '\'') << " shell hook --shell "
                << shell << " --prefix " << std::quoted(cyg_env_prefix, '\'') << ")\"\n";
        content << "# <<< mamba initialize <<<\n";
        return content.str();

#else

        fs::path env_bin = env_prefix / "bin";

        content << "# >>> mamba initialize >>>\n";
        content << "# !! Contents within this block are managed by 'mamba init' !!\n";
        content << "export MAMBA_EXE=" << mamba_exe << ";\n";
        content << "export MAMBA_ROOT_PREFIX=" << env_prefix << ";\n";
        content << "__mamba_setup=\"$(" << std::quoted(mamba_exe.string(), '\'')
                << " shell hook --shell " << shell << " --prefix "
                << std::quoted(env_prefix.string(), '\'') << " 2> /dev/null)\"\n";
        content << "if [ $? -eq 0 ]; then\n";
        content << "    eval \"$__mamba_setup\"\n";
        content << "else\n";
        content << "    if [ -f " << (env_prefix / "etc" / "profile.d" / "mamba.sh")
                << " ]; then\n";
        content << "        . " << (env_prefix / "etc" / "profile.d" / "mamba.sh") << "\n";
        content << "    else\n";
        content << "        export PATH=\"" << env_bin.c_str() << ":$PATH\"\n";
        content << "    fi\n";
        content << "fi\n";
        content << "unset __mamba_setup\n";
        content << "# <<< mamba initialize <<<\n";

        return content.str();

#endif
    }

    std::string xonsh_content(const fs::path& env_prefix,
                              const std::string& shell,
                              const fs::path& mamba_exe)
    {
        std::stringstream content;
        std::string s_mamba_exe;

        if (on_win)
        {
            s_mamba_exe = native_path_to_unix(mamba_exe.string());
        }
        else
        {
            s_mamba_exe = mamba_exe;
        }

        content << "# >>> mamba initialize >>>\n";
        content << "# !! Contents within this block are managed by 'mamba init' !!\n";
        content << "$MAMBA_EXE = " << mamba_exe << "\n";
        content << "$MAMBA_ROOT_PREFIX = " << env_prefix << "\n";
        content << "import sys as _sys\n";
        content << "from types import ModuleType as _ModuleType\n";
        content << "_mod = _ModuleType(\"xontrib.mamba\",\n";
        content << "                   \'Autogenerated from $(" << mamba_exe
                << " shell hook -s xonsh -p " << env_prefix << ")\')\n";
        content << "__xonsh__.execer.exec($(" << mamba_exe << " \"shell\" \"hook\" -s xonsh -p "
                << env_prefix << "),\n";
        content << "                      glbs=_mod.__dict__,\n";
        content << "                      filename=\'$(" << mamba_exe << " shell hook -s xonsh -p "
                << env_prefix << ")\')\n";
        content << "_sys.modules[\"xontrib.mamba\"] = _mod\n";
        content << "del _sys, _mod, _ModuleType\n";
        content << "# <<< mamba initialize <<<\n";
        return content.str();
    }

    bool modify_rc_file(const fs::path& file_path,
                        const fs::path& conda_prefix,
                        const std::string& shell,
                        const fs::path& mamba_exe)
    {
        Console::stream() << "Modifiying RC file " << file_path
                          << "\nGenerating config for root prefix " << termcolor::bold
                          << conda_prefix << termcolor::reset
                          << "\nSetting mamba executable to: " << termcolor::bold << mamba_exe
                          << termcolor::reset;

        // TODO do we need binary or not?
        std::string rc_content = read_contents(file_path, std::ios::in);
        std::string original_content = rc_content;
        std::string conda_init_content;
        if (shell == "xonsh")
        {
            conda_init_content = xonsh_content(conda_prefix, shell, mamba_exe);
        }
        else
        {
            conda_init_content = rcfile_content(conda_prefix, shell, mamba_exe);
        }

        Console::stream() << "Adding (or replacing) the following in your " << file_path
                          << " file\n"
                          << termcolor::colorize << termcolor::green << conda_init_content
                          << termcolor::reset;

        std::string result
            = std::regex_replace(rc_content, CONDA_INITIALIZE_RE_BLOCK, conda_init_content);

        if (result.find("# >>> mamba initialize >>>") == result.npos)
        {
            std::ofstream rc_file(file_path, std::ios::app | std::ios::binary);
            rc_file << std::endl << conda_init_content;
        }
        else
        {
            std::ofstream rc_file(file_path, std::ios::out | std::ios::binary);
            rc_file << result;
        }
        return true;
    }

    std::string get_hook_contents(const std::string& shell)
    {
        fs::path exe = get_self_exe_path();
        if (shell == "zsh" || shell == "bash" || shell == "posix")
        {
            std::string contents = mamba_sh;
            replace_all(contents, "$MAMBA_EXE", exe.string());
            return contents;
        }
        else if (shell == "xonsh")
        {
            std::string contents = mamba_xsh;
            replace_all(contents, "$MAMBA_EXE", exe.string());
            return contents;
        }
        else if (shell == "powershell")
        {
            std::stringstream contents;
            contents << "$Env:MAMBA_EXE=" << exe << "\n";
            std::string psm1 = mamba_psm1;
            psm1 = psm1.substr(0, psm1.find("## EXPORTS ##"));
            contents << psm1;
            return contents.str();
        }
        else if (shell == "cmd.exe")
        {
            init_root_prefix_cmdexe(Context::instance().root_prefix);
            std::cout
                << termcolor::red << "Hook installed, now 'manually' execute:\n\n"
                << termcolor::reset << "       CALL "
                << std::quoted(
                       (Context::instance().root_prefix / "condabin" / "mamba_hook.bat").string())
                << std::endl;
        }
        return "";
    }

    void init_root_prefix_cmdexe(const fs::path& root_prefix)
    {
        fs::path exe = get_self_exe_path();
        fs::create_directories(root_prefix / "condabin");
        std::ofstream mamba_bat_f(root_prefix / "condabin" / "micromamba.bat");
        std::string mamba_bat_contents(mamba_bat);
        replace_all(mamba_bat_contents,
                    std::string("__MAMBA_INSERT_ROOT_PREFIX__"),
                    std::string("@SET \"MAMBA_ROOT_PREFIX=" + root_prefix.string() + "\""));
        replace_all(mamba_bat_contents,
                    std::string("__MAMBA_INSERT_MAMBA_EXE__"),
                    std::string("@SET \"MAMBA_EXE=" + exe.string() + "\""));

        mamba_bat_f << mamba_bat_contents;
        std::ofstream _mamba_activate_bat_f(root_prefix / "condabin" / "_mamba_activate.bat");
        _mamba_activate_bat_f << _mamba_activate_bat;

        std::string hook_content = mamba_hook_bat;
        replace_all(hook_content,
                    std::string("__MAMBA_INSERT_MAMBA_EXE__"),
                    std::string("@SET \"MAMBA_EXE=" + exe.string() + "\""));

        std::ofstream mamba_hook_bat_f(root_prefix / "condabin" / "mamba_hook.bat");
        mamba_hook_bat_f << hook_content;
    }

    void init_root_prefix(const std::string& shell, const fs::path& root_prefix)
    {
        Context::instance().root_prefix = root_prefix;
        if (fs::exists(root_prefix))
        {
            if (!Console::prompt("Prefix at " + root_prefix.string()
                                 + " already exists, use as root prefix?"))
            {
                Console::print("OK, exiting.");
                exit(0);
            }
        }

        if (shell == "zsh" || shell == "bash" || shell == "posix")
        {
            PosixActivator a;
            auto sh_source_path = a.hook_source_path();
            fs::create_directories(sh_source_path.parent_path());
            std::ofstream sh_file(sh_source_path);
            sh_file << mamba_sh;
        }
        if (shell == "xonsh")
        {
            XonshActivator a;
            auto sh_source_path = a.hook_source_path();
            fs::create_directories(sh_source_path.parent_path());
            std::ofstream sh_file(sh_source_path);
            sh_file << mamba_xsh;
        }
        else if (shell == "cmd.exe")
        {
            init_root_prefix_cmdexe(root_prefix);
        }
        else if (shell == "powershell")
        {
            fs::create_directories(root_prefix / "condabin");
            std::ofstream mamba_hook_f(root_prefix / "condabin" / "mamba_hook.ps1");
            mamba_hook_f << mamba_hook_ps1;
            std::ofstream mamba_psm1_f(root_prefix / "condabin" / "Mamba.psm1");
            mamba_psm1_f << mamba_psm1;
        }
    }

    std::string powershell_contents(const fs::path& conda_prefix)
    {
        fs::path self_exe = get_self_exe_path();

        std::stringstream out;

        out << "#region mamba initialize\n";
        out << "# !! Contents within this block are managed by 'mamba shell init' !!\n";
        out << "$Env:MAMBA_ROOT_PREFIX = " << conda_prefix << "\n";
        out << "$Env:MAMBA_EXE = " << self_exe << "\n";
        out << "(& " << self_exe << " 'shell' 'hook' -s 'powershell' -p " << conda_prefix
            << ") | Out-String | Invoke-Expression\n";
        out << "#endregion\n";
        return out.str();
    }

    bool init_powershell(const fs::path& profile_path, const fs::path& conda_prefix, bool reverse)
    {
        // NB: the user may not have created a profile. We need to check
        //     if the file exists first.
        std::string profile_content, profile_original_content;
        if (fs::exists(profile_path))
        {
            profile_content = read_contents(profile_path);
            profile_original_content = profile_content;
        }

        if (reverse)
        {
            profile_content = std::regex_replace(profile_content, CONDA_INITIALIZE_PS_RE_BLOCK, "");
        }
        else
        {
            // # Find what content we need to add.
            std::string conda_init_content = powershell_contents(conda_prefix);
            std::cout << "Adding: \n" << conda_init_content << std::endl;

            Console::stream() << "Adding (or replacing) the following in your " << profile_path
                              << " file\n"
                              << termcolor::colorize << termcolor::green << conda_init_content
                              << termcolor::reset;

            if (profile_content.find("#region mamba initialize") == profile_content.npos)
            {
                profile_content += "\n" + conda_init_content + "\n";
            }
            else
            {
                profile_content = std::regex_replace(
                    profile_content, CONDA_INITIALIZE_PS_RE_BLOCK, conda_init_content);
            }
        }
        if (profile_content != profile_original_content)
        {
            if (!Context::instance().dry_run)
            {
                if (!fs::exists(profile_path.parent_path()))
                {
                    fs::create_directories(profile_path.parent_path());
                }
                std::ofstream out(profile_path);
                out << profile_content;
                return true;
            }
        }
        return false;
    }

    void init_shell(const std::string& shell, const fs::path& conda_prefix)
    {
        init_root_prefix(shell, conda_prefix);
        auto mamba_exe = get_self_exe_path();
        fs::path home = env::home_directory();
        if (shell == "bash")
        {
            fs::path bashrc_path = (on_mac || on_win) ? home / ".bash_profile" : home / ".bashrc";
            modify_rc_file(bashrc_path, conda_prefix, shell, mamba_exe);
        }
        else if (shell == "zsh")
        {
            fs::path zshrc_path = home / ".zshrc";
            modify_rc_file(zshrc_path, conda_prefix, shell, mamba_exe);
        }
        else if (shell == "xonsh")
        {
            fs::path xonshrc_path = home / ".xonshrc";
            // std::cout << xonsh_content(conda_prefix, shell, mamba_exe);
            modify_rc_file(xonshrc_path, conda_prefix, shell, mamba_exe);
        }
        else if (shell == "cmd.exe")
        {
#ifndef _WIN32
            throw std::runtime_error("CMD.EXE can only be initialized on Windows.");
#else
            init_cmd_exe_registry(L"Software\\Microsoft\\Command Processor", conda_prefix, false);
#endif
        }
        else if (shell == "powershell")
        {
            std::string profile_var("$PROFILE.CurrentUserAllHosts");
            // if (for_system)
            //     profile = "$PROFILE.AllUsersAllHosts"

            // There's several places PowerShell can store its path, depending
            // on if it's Windows PowerShell, PowerShell Core on Windows, or
            // PowerShell Core on macOS/Linux. The easiest way to resolve it is to
            // just ask different possible installations of PowerShell where their
            // profiles are.

            auto find_powershell_paths = [&profile_var](const std::string& exe) -> std::string {
                try
                {
                    std::string out, err;
                    auto [status, ec] = reproc::run(
                        std::vector<std::string>{ exe, "-NoProfile", "-Command", profile_var },
                        reproc::options{},
                        reproc::sink::string(out),
                        reproc::sink::string(err));
                    if (ec)
                    {
                        throw std::runtime_error(ec.message());
                    }
                    return std::string(strip(out));
                }
                catch (...)
                {
                    return "";
                }
            };

            std::string profile_path, exe;
            for (auto& iter_exe : std::vector<std::string>{ "powershell", "pwsh", "pwsh-preview" })
            {
                auto res = find_powershell_paths(iter_exe);
                if (!res.empty())
                {
                    profile_path = res;
                    exe = iter_exe;
                    break;
                }
            }
            std::cout << "Found powershell at " << exe << " and user profile at " << profile_path
                      << std::endl;

            init_powershell(profile_path, conda_prefix, false);
        }
        else
        {
            throw std::runtime_error("Support for other shells not yet implemented.");
        }
    }
}
