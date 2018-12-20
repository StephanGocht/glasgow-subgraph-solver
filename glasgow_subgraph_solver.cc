/* vim: set sw=4 sts=4 et foldmethod=syntax : */

#include "formats/read_file_format.hh"
#include "restarts.hh"
#include "solver.hh"
#include "verify.hh"

#include <boost/program_options.hpp>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>

#include <unistd.h>

namespace po = boost::program_options;

using std::boolalpha;
using std::cerr;
using std::cout;
using std::endl;
using std::exception;
using std::function;
using std::localtime;
using std::make_pair;
using std::make_unique;
using std::put_time;
using std::string;

using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::operator""s;
using std::chrono::seconds;
using std::chrono::steady_clock;
using std::chrono::system_clock;

auto main(int argc, char * argv[]) -> int
{
    try {
        po::options_description display_options{ "Program options" };
        display_options.add_options()
            ("help",                                         "Display help information")
            ("timeout",            po::value<int>(),         "Abort after this many seconds")
            ("format",             po::value<string>(),      "Specify input file format (auto, lad, labelledlad, dimacs)")
            ("pattern-format",     po::value<string>(),      "Specify input file format just for the pattern graph")
            ("target-format",      po::value<string>(),      "Specify input file format just for the target graph")
            ("induced",                                      "Find an induced mapping")
            ("noninjective",                                 "Drop the injectivity requirement")
            ("enumerate",                                    "Count the number of solutions");

        po::options_description configuration_options{ "Advanced configuration options" };
        configuration_options.add_options()
            ("distance-filtering",                           "Do distance filtering (only for noninjective)")
            ("prefer-injectivity",                           "Prefer injectivity for value-ordering (only for noninjective)")
            ("nogood-size-limit",  po::value<int>(),         "Maximum size of nogood to generate (0 disables nogoods)")
            ("restarts-constant",  po::value<int>(),         "How often to perform restarts (0 disables restarts)")
            ("restart-timer",      po::value<int>(),         "Also restart after this many milliseconds (0 disables)")
            ("geometric-restarts", po::value<double>(),      "Use geometric restarts with the specified multiplier (default is Luby)")
            ("value-ordering",     po::value<string>(),      "Specify value-ordering heuristic (biased / degree / antidegree / random)");
        display_options.add(configuration_options);

        po::options_description all_options{ "All options" };
        all_options.add_options()
            ("pattern-file", "Specify the pattern file")
            ("target-file",  "Specify the target file")
            ;

        all_options.add(display_options);

        po::positional_options_description positional_options;
        positional_options
            .add("pattern-file", 1)
            .add("target-file", 1)
            ;

        po::variables_map options_vars;
        po::store(po::command_line_parser(argc, argv)
                .options(all_options)
                .positional(positional_options)
                .run(), options_vars);
        po::notify(options_vars);

        /* --help? Show a message, and exit. */
        if (options_vars.count("help")) {
            cout << "Usage: " << argv[0] << " [options] pattern target" << endl;
            cout << endl;
            cout << display_options << endl;
            return EXIT_SUCCESS;
        }

        /* No algorithm or no input file specified? Show a message and exit. */
        if (! options_vars.count("pattern-file") || ! options_vars.count("target-file")) {
            cout << "Usage: " << argv[0] << " [options] pattern target" << endl;
            return EXIT_FAILURE;
        }

        /* Figure out what our options should be. */
        Params params;

        params.distance_filtering = options_vars.count("distance-filtering");
        params.prefer_injectivity = options_vars.count("prefer-injectivity");
        params.noninjective = options_vars.count("noninjective");
        params.induced = options_vars.count("induced");
        params.enumerate = options_vars.count("enumerate");

        if (options_vars.count("nogood-size-limit"))
            params.nogood_size_limit = options_vars["nogood-size-limit"].as<int>();

        if (params.enumerate)
            params.restarts_schedule = make_unique<NoRestartsSchedule>();
        else if (options_vars.count("geometric-restarts")) {
            double initial_value = GeometricRestartsSchedule::default_initial_value;
            double multiplier = options_vars["geometric-restarts"].as<double>();
            if (options_vars.count("restarts-constant"))
                initial_value = options_vars["restarts-constant"].as<int>();
            params.restarts_schedule = make_unique<GeometricRestartsSchedule>(initial_value, multiplier);
        }
        else {
            long long multiplier = LubyRestartsSchedule::default_multiplier;
            if (options_vars.count("restarts-constant"))
                multiplier = options_vars["restarts-constant"].as<int>();
            params.restarts_schedule = make_unique<LubyRestartsSchedule>(multiplier);
        }

        if (options_vars.count("value-ordering")) {
            string value_ordering_heuristic = options_vars["value-ordering"].as<string>();
            if (value_ordering_heuristic == "biased")
                params.value_ordering_heuristic = ValueOrdering::Biased;
            else if (value_ordering_heuristic == "degree")
                params.value_ordering_heuristic = ValueOrdering::Degree;
            else if (value_ordering_heuristic == "antidegree")
                params.value_ordering_heuristic = ValueOrdering::AntiDegree;
            else if (value_ordering_heuristic == "random")
                params.value_ordering_heuristic = ValueOrdering::Random;
            else {
                cerr << "Unknown value-ordering heuristic '" << value_ordering_heuristic << "'" << endl;
                return EXIT_FAILURE;
            }
        }

        char hostname_buf[255];
        if (0 == gethostname(hostname_buf, 255))
            cout << "hostname = " << string(hostname_buf) << endl;
        cout << "commandline =";
        for (int i = 0 ; i < argc ; ++i)
            cout << " " << argv[i];
        cout << endl;

        auto started_at = system_clock::to_time_t(system_clock::now());
        cout << "started_at = " << put_time(localtime(&started_at), "%F %T") << endl;

        /* Read in the graphs */
        string default_format_name = options_vars.count("format") ? options_vars["format"].as<string>() : "auto";
        string pattern_format_name = options_vars.count("pattern-format") ? options_vars["pattern-format"].as<string>() : default_format_name;
        string target_format_name = options_vars.count("target-format") ? options_vars["target-format"].as<string>() : default_format_name;
        auto graphs = make_pair(
            read_file_format(pattern_format_name, options_vars["pattern-file"].as<string>()),
            read_file_format(target_format_name, options_vars["target-file"].as<string>()));

        cout << "pattern_file = " << options_vars["pattern-file"].as<string>() << endl;
        cout << "target_file = " << options_vars["target-file"].as<string>() << endl;

        /* Prepare and start timeout */
        params.timeout = make_unique<Timeout>(options_vars.count("timeout") ? seconds{ options_vars["timeout"].as<int>() } : 0s);

        /* Start the clock */
        params.start_time = steady_clock::now();

        auto result = solve_subgraph_problem(graphs, params);

        /* Stop the clock. */
        auto overall_time = duration_cast<milliseconds>(steady_clock::now() - params.start_time);

        params.timeout->stop();

        cout << "status = ";
        if (params.timeout->aborted())
            cout << "aborted";
        else if ((! result.mapping.empty()) || (params.enumerate && result.solution_count > 0))
            cout << "true";
        else
            cout << "false";
        cout << endl;

        if (params.enumerate)
            cout << "solution_count = " << result.solution_count << endl;

        cout << "nodes = " << result.nodes << endl;
        cout << "propagations = " << result.propagations << endl;

        if (! result.mapping.empty()) {
            cout << "mapping = ";
            for (auto v : result.mapping)
                cout << "(" << graphs.first.vertex_name(v.first) << " -> " << graphs.second.vertex_name(v.second) << ") ";
            cout << endl;
        }

        cout << "runtime = " << overall_time.count() << endl;

        for (const auto & s : result.extra_stats)
            cout << s << endl;

        verify(graphs, params, result);

        return EXIT_SUCCESS;
    }
    catch (const GraphFileError & e) {
        cerr << "Error: " << e.what() << endl;
        cerr << "Maybe try specifying one of --format, --pattern-format, or --target-format?" << endl;
        return EXIT_FAILURE;
    }
    catch (const po::error & e) {
        cerr << "Error: " << e.what() << endl;
        cerr << "Try " << argv[0] << " --help" << endl;
        return EXIT_FAILURE;
    }
    catch (const exception & e) {
        cerr << "Error: " << e.what() << endl;
        return EXIT_FAILURE;
    }
}

