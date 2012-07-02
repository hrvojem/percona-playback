/* BEGIN LICENSE
 * Copyright (C) 2011-2012 Percona Inc.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 * END LICENSE */

#include "config.h"

#include <stdlib.h>
#include <string>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <assert.h>
#include "percona_playback.h"
#include "version.h"
#include "query_log/query_log.h"

#include <boost/program_options.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/foreach.hpp>

#include <percona_playback/plugin.h>
#include <percona_playback/gettext.h>

#include <vector>

namespace po= boost::program_options;

percona_playback::DBClientPlugin *g_dbclient_plugin= NULL;
percona_playback::InputPlugin *g_input_plugin= NULL;
unsigned int g_db_thread_queue_depth;

struct percona_playback_st
{
  const char *name;
  unsigned int loop;
};

percona_playback_st *percona_playback_create(const char *name)
{
  percona_playback_st *the_percona_playback=
    static_cast<percona_playback_st *>(malloc(sizeof(percona_playback_st)));
  assert(the_percona_playback);
  the_percona_playback->name= name;
  return the_percona_playback;
}

void percona_playback_destroy(percona_playback_st **the_percona_playback)
{
  if (the_percona_playback)
  {
    free(*the_percona_playback);
    *the_percona_playback= NULL;
  }
}

const char *percona_playback_get_name(const percona_playback_st *the_percona_playback)
{
  if (the_percona_playback)
    return the_percona_playback->name;
  return NULL;
}

static void version()
{
  std::cerr << PACKAGE << std::endl
	    << "Version: " PACKAGE_VERSION
	    << "-" << PERCONA_PLAYBACK_VERSION_ID
	    << "-" << PERCONA_PLAYBACK_RELEASE_COMMENT
	    << std::endl;
}

static void help(po::options_description &options_description)
{
    version();
    std::cerr << std::endl;
    std::cerr << options_description << std::endl;
    std::cerr << std::endl;
    std::cerr << _("Bugs: ") << PACKAGE_BUGREPORT << std::endl;
    std::cerr << _("Loaded plugins: ");
    BOOST_FOREACH(const std::string &plugin_name, percona_playback::PluginRegistry::singleton().loaded_plugin_names)
    {
      std::cerr << plugin_name << " ";
    }

    std::cerr << std::endl;

    std::cerr << std::endl << _("Loaded DB Plugins: ");
    for(percona_playback::PluginRegistry::DBClientPluginMap::iterator it= percona_playback::PluginRegistry::singleton().dbclient_plugins.begin();
	it != percona_playback::PluginRegistry::singleton().dbclient_plugins.end();
	it++)
    {
      std::cerr << it->first << " ";
    }
    std::cerr << std::endl;
    std::cerr << std::endl;

    assert(g_dbclient_plugin);
    std::cerr << _("Selected DB Plugin: ") << g_dbclient_plugin->name << std::endl;

    std::cerr << std::endl << _("Loaded Input Plugins: ");

    BOOST_FOREACH(const percona_playback::PluginRegistry::InputPluginPair &pp,
		  percona_playback::PluginRegistry::singleton().input_plugins)
    {
      std::cerr << pp.first << " ";
    }

    std::cerr << std::endl;
    std::cerr << std::endl;

    assert(g_input_plugin);
    std::cerr << _("Selected Input Plugin: ")
              << g_input_plugin->name
              << std::endl;
}

int percona_playback_argv(percona_playback_st *the_percona_playback,
			  int argc, char** argv)
{
  percona_playback::load_plugins();

  po::options_description general_options(_("General options"));
  general_options.add_options()
    ("help",    "Display this message")
    ("version", "Display version information")
    ("loop", po::value<unsigned int>(), "Do the whole run N times")
    ;

  po::options_description db_options("Database Options");
  db_options.add_options()
    ("db-plugin", po::value<std::string>(), "Database plugin")
    ("input-plugin", po::value<std::string>(), "Input plugin")
    ("queue-depth", po::value<unsigned int>(),
     "Queue depth for DB executor (thread). The larger this number the"
     " greater the played-back workload can deviate from the original workload"
     " as some connections may be up to queue-depth behind. (default 1)")
    ;

  std::string basic_usage;
  basic_usage= "USAGE: " + std::string(PACKAGE) + " [General Options]";
  po::options_description options_description(basic_usage);
  options_description.add(general_options);
  options_description.add(db_options);

  BOOST_FOREACH(const percona_playback::PluginRegistry::PluginPair pp,
		percona_playback::PluginRegistry::singleton().all_plugins)
  {
    po::options_description *plugin_opts= pp.second->getProgramOptions();

    if (plugin_opts != NULL)
      options_description.add(*plugin_opts);
  }

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, options_description), vm);
  po::notify(vm);

  if (vm.count("db-plugin"))
  {
    g_dbclient_plugin= percona_playback::PluginRegistry::singleton().dbclient_plugins[vm["db-plugin"].as<std::string>()];
    if (g_dbclient_plugin == NULL)
    {
      std::cerr << "Invalid DB Plugin" << std::endl;
      return -1;
    }
  }
  else
  {
    g_dbclient_plugin= percona_playback::PluginRegistry::singleton().dbclient_plugins["libmysqlclient"];
    if (g_dbclient_plugin == NULL)
      g_dbclient_plugin= percona_playback::PluginRegistry::singleton().
        dbclient_plugins["null"];
    if (g_dbclient_plugin == NULL)
    {
      fprintf(stderr, gettext("Invalid DB plugin\n"));
      return -1;
    }
  }
  g_dbclient_plugin->active= true;

  if (vm.count("input-plugin"))
  {
    g_input_plugin= 
      percona_playback::PluginRegistry::singleton().input_plugins[
        vm["input-plugin"].as<std::string>()
      ];
    if (g_input_plugin == NULL)
    {
      std::cerr << "Invalid Input Plugin" << std::endl;
      return -1;
    }
  }
  else
  {
    g_input_plugin=
      percona_playback::PluginRegistry::singleton().input_plugins["query-log"];
    if (g_dbclient_plugin == NULL)
    {
      fprintf(stderr, gettext("Invalid Input plugin\n"));
      return -1;
    }
  }
  g_input_plugin->active= true;

  if (vm.count("help") || argc==1)
  {
    help(options_description);
    return 1;
  }

  if (vm.count("version"))
  {
    version();
    return 2;
  }

  /*
    Process plugin options after "help" processing to avoid
    required options requests in "help" message.
  */
  BOOST_FOREACH(const percona_playback::PluginRegistry::PluginPair &pp,
		percona_playback::PluginRegistry::singleton().all_plugins)
  {
    if (pp.second->processOptions(vm))
      return -1;
  }

  if (vm.count("loop"))
  {
    the_percona_playback->loop= vm["loop"].as<unsigned int>();
  }
  else
    the_percona_playback->loop= 1;

  if (vm.count("queue-depth"))
  {
    g_db_thread_queue_depth= vm["queue-depth"].as<unsigned int>();
  }
  else
    g_db_thread_queue_depth= 1;

  return 0;
}

static
percona_playback_run_result *
create_percona_playback_run_result()
{
  percona_playback_run_result *r=
    static_cast<struct percona_playback_run_result *>(
      malloc(sizeof(struct percona_playback_run_result)));
  assert(r);
  r->err= 0;
  r->n_log_entries= 0;
  r->n_queries= 0;
  return r;
}

struct percona_playback_run_result *percona_playback_run(const percona_playback_st *the_percona_playback)
{
  percona_playback_run_result *r= create_percona_playback_run_result();
  assert(g_dbclient_plugin);

  std::cerr << "Database Plugin: " << g_dbclient_plugin->name << std::endl;
  std::cerr << " Running..." << std::endl;

  g_input_plugin->run(*r);

  BOOST_FOREACH(const percona_playback::PluginRegistry::ReportPluginPair pp,
		  percona_playback::PluginRegistry::singleton().report_plugins)
  {
    pp.second->print_report();
  }

  return r;
}

int percona_playback_run_all(const percona_playback_st *the_percona_playback)
{
  struct percona_playback_run_result *r;

  for(unsigned int run=0; run < the_percona_playback->loop; run++)
  {
    if (the_percona_playback->loop > 1)
    {
      fprintf(stderr, "Run %u of %u\n", run+1, the_percona_playback->loop);
    }
    r= percona_playback_run(the_percona_playback);
    if (r->err != 0)
    {
      free(r);
      return -1;
    }
    free(r);
  }

  return 0;
}
