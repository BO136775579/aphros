// Created by Petr Karnakov on 25.04.2018
// Copyright 2018 ETH Zurich

#ifdef _OPENMP
#include <omp.h>
#endif

#include <set>

#include "distrsolver.h"
#include "linear/hypresub.h"
#include "parse/argparse.h"
#include "util/git.h"
#include "util/subcomm.h"

static void RunKernelOpenMP(
    MPI_Comm comm_world, MPI_Comm comm_omp, MPI_Comm comm_master,
    std::function<void(MPI_Comm, Vars&)> kernel, Vars& var) {
  int rank_omp;
  MPI_Comm_rank(comm_omp, &rank_omp);

  Histogram hist(comm_world, "runkernelOMP", var.Int["histogram"]);
  HypreSub::InitServer(comm_world, comm_omp);
  if (rank_omp == 0) {
    kernel(comm_master, var);
    HypreSub::StopServer();
  } else {
    HypreSub::RunServer(hist);
  }
}

int RunMpi0(
    int argc, const char** argv, std::function<void(MPI_Comm, Vars&)> kernel) {
#ifdef _OPENMP
  omp_set_dynamic(0);
#endif
  char string[MPI_MAX_ERROR_STRING];
  int errorcode;
  int prov;
  int resultlen;
  if ((errorcode = MPI_Init_thread(
           &argc, (char***)&argv, MPI_THREAD_MULTIPLE, &prov)) != MPI_SUCCESS) {
    MPI_Error_string(errorcode, string, &resultlen);
    throw std::runtime_error(FILELINE + ": mpi failed: " + string);
  }
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  bool isroot = (!rank);

  ArgumentParser parser("Distributed solver", isroot);
  parser.AddSwitch({"--verbose", "-v"}).Help("Print initial configuration");
  parser.AddSwitch({"--version"}).Help("Print version");
  parser.AddVariable<std::string>("config", "a.conf")
      .Help("Path to configuration file");
  auto args = parser.ParseArgs(argc, argv);
  if (const int* p = args.Int.Find("EXIT")) {
    return *p;
  }

  const bool verbose = args.Int["verbose"];
  const std::string config = args.String["config"];
  if (args.Int["version"] && isroot) {
    std::cerr << "aphros " << GetGitRev() << "\nmsg: " << GetGitMsg()
              << "\ndiff: " << GetGitDiff() << '\n';
  }

  if (verbose && isroot) {
    std::cerr << "Loading config from '" << config << "'" << std::endl;
  }

  Vars var; // parameter storage
  Parser ip(var); // parser

  ip.ParseFile(config);

  // Print vars on root
  if (verbose && isroot) {
    std::cerr << "\n=== config begin ===\n";
    ip.PrintAll(std::cerr);
    std::cerr << "=== config end ===\n\n";
  }

  const std::string backend = var.String["backend"];

  if (backend == "local") {
    MPI_Comm comm;
    MPI_Comm_split(MPI_COMM_WORLD, rank, rank, &comm);
    if (rank == 0) {
      RunKernelOpenMP(comm, comm, comm, kernel, var);
    }
  } else {
    bool openmp = var.Int["openmp"];
    if (openmp) {
      MPI_Comm comm_world;
      MPI_Comm comm_omp;
      MPI_Comm comm_master;
      SubComm(comm_world, comm_omp, comm_master);
      if (var.Int["verbose_openmp"]) {
        PrintStats(comm_world, comm_omp, comm_master);
      }
      RunKernelOpenMP(comm_world, comm_omp, comm_master, kernel, var);
    } else {
      MPI_Comm comm = MPI_COMM_WORLD;
      MPI_Comm comm_omp;
      MPI_Comm_split(comm, rank, rank, &comm_omp);
      RunKernelOpenMP(comm, comm_omp, comm, kernel, var);
    }
  }

  if (isroot) {
    if (var.Int("verbose_conf_reads", 0)) {
      auto print_reads = [&var](const auto& map) {
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
          const auto key = it->first;
          std::cout << map.GetReads(key) << ' ' << map.GetTypeName() << ' '
                    << key << '\n';
        }
      };
      std::cout << "Number of accesses to configuration variables\n";
      var.ForEachMap(print_reads);
    }
    if (var.Int("verbose_conf_unused", 0)) {
      const std::string path = var.String["conf_unused_ignore_path"];
      std::set<std::string> ignore;
      if (path != "") {
        Vars vign;
        Parser parser(vign);
        std::ifstream f(path);
        parser.ParseStream(f);
        vign.ForEachMap([&ignore](const auto& map) {
          for (auto it = map.cbegin(); it != map.cend(); ++it) {
            ignore.insert(it->first);
          }
        });
      }
      std::cout << "Unused configuration variables:\n";
      var.ForEachMap([&var, &ignore](const auto& map) {
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
          const auto key = it->first;
          if (map.GetReads(key) == 0 && !ignore.count(key)) {
            std::cout << map.GetTypeName() << ' ' << key << '\n';
          }
        }
      });
    }
  }

  MPI_Finalize();
  return 0;
}

int RunMpi(
    int argc, const char** argv, std::function<void(MPI_Comm, Vars&)> kernel) {
  int status;
  try {
    status = RunMpi0(argc, argv, kernel);
  } catch (const std::exception& e) {
    status = 1;
    std::cerr << "\nabort after throwing exception\n" << e.what() << '\n';
    throw e;
  }
  return status;
}
