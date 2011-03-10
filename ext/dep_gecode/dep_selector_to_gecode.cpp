#include <gecode/driver.hh>
#include <gecode/int.hh>
#include <gecode/minimodel.hh>
#include <gecode/gist.hh>
#include <gecode/search.hh>

#include "dep_selector_to_gecode.h"

#include <limits>
#include <iostream>
#include <vector>

//#define DEBUG
//#define USE_DUMB_BRANCHING
#define VECTOR_CONSTRAIN
#define COMPUTE_LINEAR_AGGREGATE

using namespace Gecode;
const int VersionProblem::UNRESOLVED_VARIABLE = INT_MIN;
const int VersionProblem::MIN_TRUST_LEVEL = 0;
const int VersionProblem::MAX_TRUST_LEVEL = 10;
const int VersionProblem::MAX_PREFERRED_WEIGHT = 10;

VersionProblem::VersionProblem(int packageCount)
  : size(packageCount), finalized(false), cur_package(0), package_versions(*this, packageCount), 
    disabled_package_variables(*this, packageCount, 0, 1), total_disabled(*this, 0, packageCount*MAX_TRUST_LEVEL),
    total_required_disabled(*this, 0, packageCount), total_induced_disabled(*this, 0, packageCount), 
    total_suspicious_disabled(*this, 0, packageCount),
    is_required(new int[packageCount]),
    is_suspicious(new int[packageCount]),
    at_latest(*this, packageCount, 0, 1),
    // These domains could be narrowed a bit; check later
    total_preferred_at_latest(*this, -packageCount*MAX_PREFERRED_WEIGHT, packageCount*MAX_PREFERRED_WEIGHT), 
    total_not_preferred_at_latest(*this, -packageCount, packageCount), 
    preferred_at_latest_weights(new int[packageCount]),
    aggregate_cost(*this, INT_MIN/2, INT_MAX/2) //-packageCount*packageCount*MAX_TRUST_LEVEL*MAX_PREFERRED_WEIGHT, packageCount*packageCount*MAX_TRUST_LEVEL*MAX_PREFERRED_WEIGHT)
{
  for (int i = 0; i < packageCount; i++)
  {
    preferred_at_latest_weights[i] = 0;
    is_required[i] = 0;
    is_suspicious[i] = 0;
  }
}

VersionProblem::VersionProblem(bool share, VersionProblem & s) 
  : Space(share, s), size(s.size),
    finalized(s.finalized), cur_package(s.cur_package),
    disabled_package_variables(s.disabled_package_variables), total_disabled(s.total_disabled),
    total_required_disabled(s.total_required_disabled), total_induced_disabled(s.total_induced_disabled), 
    total_suspicious_disabled(s.total_suspicious_disabled),
    is_required(NULL), is_suspicious(NULL),
    at_latest(s.at_latest),
    total_preferred_at_latest(s.total_preferred_at_latest), 
    total_not_preferred_at_latest(s.total_preferred_at_latest), 
    preferred_at_latest_weights(NULL),
    aggregate_cost(s.aggregate_cost)
{
  package_versions.update(*this, share, s.package_versions);
  disabled_package_variables.update(*this, share, s.disabled_package_variables);
  total_disabled.update(*this, share, s.total_disabled);
  total_required_disabled.update(*this, share, s.total_required_disabled);
  total_induced_disabled.update(*this, share, s.total_induced_disabled);
  total_suspicious_disabled.update(*this, share, s.total_suspicious_disabled);
  at_latest.update(*this, share, s.at_latest);
  total_preferred_at_latest.update(*this, share, s.total_preferred_at_latest);
  total_not_preferred_at_latest.update(*this, share, s.total_not_preferred_at_latest);
  aggregate_cost.update(*this, share, s.aggregate_cost);
}

// Support for gecode
Space* VersionProblem::copy(bool share)
{
  return new VersionProblem(share,*this);
}

VersionProblem::~VersionProblem() 
{
  delete[] preferred_at_latest_weights;
  delete[] is_required;
  delete[] is_suspicious;
}

int VersionProblem::Size() 
{
  return size;
}

int VersionProblem::PackageCount() 
{
  return cur_package;
}

int
VersionProblem::AddPackage(int minVersion, int maxVersion, int currentVersion) 
{
  if (cur_package == size) {
    return -1;
  }

#ifdef DEBUG
  std::cout << "Adding package id " << cur_package << '/' << size << ": min = " << minVersion << ", max = " << maxVersion << ", current version " << currentVersion << std::endl;
  std::cout.flush();    
#endif // DEBUG
  int index = cur_package;
  cur_package++;
  //  IntVar version(*this, minVersion, maxVersion);
  package_versions[index] = IntVar(*this, minVersion, maxVersion);

  // register the binding of package to version that corresponds to the package's latest
  rel(*this, package_versions[index], IRT_EQ, maxVersion, at_latest[index]);

  return index;
}

bool 
VersionProblem::AddVersionConstraint(int packageId, int version, 
				     int dependentPackageId, int minDependentVersion, int maxDependentVersion) 
{
  BoolVar version_match(*this, 0, 1);
  BoolVar depend_match(*this, 0, 1);
  BoolVar predicated_depend_match(*this, 0, 1);

#ifdef DEBUG
  std::cout << "Add VC for " << packageId << " @ " << version << " depPkg " << dependentPackageId;
  std::cout << " [ " << minDependentVersion << ", " << maxDependentVersion << " ]" << std::endl;
  std::cout.flush();
#endif // DEBUG


  //version_flags << version_match;
  // Constrain pred to reify package @ version
  rel(*this, package_versions[packageId], IRT_EQ, version, version_match);
  // Add the predicated version constraints imposed on dependent package

  // package_versions[dependendPackageId] in domain [minDependentVersion,maxDependentVersion] <=> depend_match
  dom(*this, package_versions[dependentPackageId], minDependentVersion, maxDependentVersion, depend_match);

  // disabled_package_variables[dependentPackageId] OR depend_match <=> predicated_depend_match
  // rel(*this, disabled_package_variables[dependentPackageId], BOT_OR, depend_match, version_match);

  rel(*this, disabled_package_variables[dependentPackageId], BOT_OR, depend_match, predicated_depend_match);
  rel(*this, version_match, BOT_IMP, predicated_depend_match, 1);  
}

void
VersionProblem::MarkPackageSuspicious(int packageId) 
{
  is_suspicious[packageId] = 1;
}

void 
VersionProblem::MarkPackageRequired(int packageId)
{
  is_required[packageId] = 1;
}

void
VersionProblem::MarkPackagePreferredToBeAtLatest(int packageId, int weight)
{
  preferred_at_latest_weights[packageId] = std::max(MAX_PREFERRED_WEIGHT, std::min(0, weight));
}

void VersionProblem::Finalize() 
{
#ifdef DEBUG
  std::cout << "Finalization Started" << std::endl;
  std::cout.flush();
#endif // DEBUG
  finalized = true;

  // Setup constraint for cost
  // We wish to minimize the total number of disabled packages, by priority ranks
  IntArgs disabled_required_weights(size, is_required);
  linear(*this, disabled_required_weights, disabled_package_variables,  IRT_EQ, total_required_disabled);
#ifdef DEBUG
  std::cout << "disabled_required_weights:            " << disabled_required_weights << std::endl;
  std::cout << "total_required_disabled:              " << total_required_disabled << std::endl;
#endif // DEBUG

  IntArgs disabled_induced_weights(size);
  for (int i = 0; i < size; i++) {
    disabled_induced_weights[i] = !(is_required[i] || is_suspicious[i]);
  }
  linear(*this, disabled_induced_weights, disabled_package_variables,  IRT_EQ, total_induced_disabled);
#ifdef DEBUG
  std::cout << "disabled_induced_weights:             " << disabled_induced_weights << std::endl;
  std::cout << "total_induced_disabled:               " << total_induced_disabled << std::endl;
#endif // DEBUG
  
  IntArgs disabled_suspicious_weights(size, is_suspicious);
  linear(*this, disabled_suspicious_weights, disabled_package_variables,  IRT_EQ, total_suspicious_disabled);
#ifdef DEBUG
  std::cout << "disabled_suspicious_weights:          " << disabled_suspicious_weights << std::endl;
  std::cout << "total_suspicious_disabled:            " << total_suspicious_disabled << std::endl;
#endif // DEBUG

  linear(*this, disabled_package_variables,  IRT_EQ, total_disabled);
#ifdef DEBUG
  std::cout << "total_disabled:                       " << total_disabled << std::endl;
#endif DEBUG

  // Setup computation for total_preferred_at_latest
  // We wish to maximize the total number of packages at their latest versions in the preferred tier of packages
  // We negate the weights in the cost function to make it fit into the context of a minimization problem.
  for (int i = 0; i < size; i++) {
    preferred_at_latest_weights[i] = -preferred_at_latest_weights[i];
  }
  IntArgs preferred_at_latest_weights_args(size, preferred_at_latest_weights);
  linear(*this, preferred_at_latest_weights_args, at_latest, IRT_EQ, total_preferred_at_latest);
#ifdef DEBUG
  std::cout << "preferred_at_latest_weights_args:     " << preferred_at_latest_weights_args << std::endl;
  std::cout << "total_preferred_at_latest:            " << total_preferred_at_latest << std::endl;
#endif DEBUG

  // Setup computation for remaining variables
  // We wish to maximize the total number of packages at their latest version in the non-preferred tier of packages
  // We negate the weights in the cost function to make it fit into the context of a minimization problem.
  IntArgs not_preferred_at_latest_weights_args = IntArgs::create(size, 0, 0);
  for (int i = 0; i < size; i++) {
    if (preferred_at_latest_weights[i] == 0) {
      not_preferred_at_latest_weights_args[i] = -1;
    }
  }
  linear(*this, not_preferred_at_latest_weights_args, at_latest, IRT_EQ, total_not_preferred_at_latest);
#ifdef DEBUG
  std::cout << "not_preferred_at_latest_weights_args: " << not_preferred_at_latest_weights_args << std::endl;
  std::cout << "total_not_preferred_at_latest:        " << total_not_preferred_at_latest << std::endl;
#endif DEBUG

#ifdef COMPUTE_LINEAR_AGGREGATE
  // Set up the aggregate cost function
  IntVar total_not_preferred_at_latest_normalized = expr(*this, total_not_preferred_at_latest - total_not_preferred_at_latest.min());
  int total_not_preferred_at_latest_range = total_not_preferred_at_latest.size();
  IntVar total_preferred_at_latest_normalized = expr(*this, total_preferred_at_latest - total_preferred_at_latest.min());
  int total_preferred_at_latest_range = total_preferred_at_latest.size();
  IntVar total_disabled_normalized = expr(*this, total_disabled - total_disabled.min());

  IntArgs aggregate_cost_weights(3, total_not_preferred_at_latest_range*total_preferred_at_latest_range, total_not_preferred_at_latest_range, 1);  
  //IntArgs aggregate_cost_weights(3, 1, 0, 0);
  IntVarArgs aggregate_vector(3);
  aggregate_vector[0] = IntVar(total_disabled); 
  aggregate_vector[1] = IntVar(total_preferred_at_latest);
  aggregate_vector[2] = IntVar(total_not_preferred_at_latest);

  linear(*this, aggregate_cost_weights, aggregate_vector, IRT_EQ, aggregate_cost);
#ifdef DEBUG
  std::cout << "aggregate_cost_weights:               " << aggregate_cost_weights << std::endl;
  std::cout << "aggregate_vector:                     " << aggregate_vector << std::endl;
  std::cout << "aggregate_cost:                       " << aggregate_cost << std::endl;
  std::cout.flush();
#endif DEBUG
#endif // COMPUTE_LINEAR_AGGREGATE

  // Cleanup
  // Assign a dummy variable to elements greater than actually used.
  for (int i = cur_package; i < size; i++) {
    package_versions[i] = IntVar(*this, -1, -1);
    disabled_package_variables[i] = BoolVar(*this, 1, 1);
  }

#ifdef USE_DUMB_BRANCHING
#  ifdef DEBUG
  std::cout << "Adding branching (POOR)" << std::endl;
  std::cout.flush();
#  endif // DEBUG
  // This branching starts as far as possible from the solution, in order to exercise the optimization functions.
  branch(*this, disabled_package_variables, INT_VAR_SIZE_MIN, INT_VAL_MAX);
  branch(*this, package_versions, INT_VAR_SIZE_MIN, INT_VAL_MIN);
  branch(*this, total_required_disabled, INT_VAL_MAX);
  branch(*this, total_induced_disabled, INT_VAL_MAX);
  branch(*this, total_suspicious_disabled, INT_VAL_MAX);
  branch(*this, total_disabled, INT_VAL_MAX);
  branch(*this, at_latest, INT_VAR_SIZE_MIN, INT_VAL_MIN);
  branch(*this, total_preferred_at_latest, INT_VAL_MIN);
  branch(*this, total_not_preferred_at_latest, INT_VAL_MIN);
#ifdef COMPUTE_LINEAR_AGGREGATE
  branch(*this, aggregate_cost, INT_VAL_MAX);
#endif // COMPUTE_LINEAR_AGGREGATE
#else // USE_DUMB_BRANCHING
#  ifdef DEBUG
  std::cout << "Adding branching (BEST)" << std::endl;
  std::cout.flush();
#  endif // DEBUG
  // This branching is meant to start with most probable solution
  branch(*this, disabled_package_variables, INT_VAR_SIZE_MIN, INT_VAL_MIN);
  branch(*this, package_versions, INT_VAR_SIZE_MIN, INT_VAL_MAX);
  branch(*this, total_required_disabled, INT_VAL_MIN);
  branch(*this, total_induced_disabled, INT_VAL_MIN);
  branch(*this, total_suspicious_disabled, INT_VAL_MIN);
  branch(*this, total_disabled, INT_VAL_MIN);
  branch(*this, at_latest, INT_VAR_SIZE_MIN, INT_VAL_MAX);
  branch(*this, total_preferred_at_latest, INT_VAL_MAX);
  branch(*this, total_not_preferred_at_latest, INT_VAL_MAX);
#ifdef COMPUTE_LINEAR_AGGREGATE
  branch(*this, aggregate_cost, INT_VAL_MIN);
#endif // COMPUTE_LINEAR_AGGREGATE
#endif // USE_DUMB_BRANCHING

#ifdef DEBUG
  std::cout << "Finalization Done" << std::endl;
  std::cout.flush();
#endif // DEBUG
}

////////////////////////////////////////////////////////////////////////
// A general note about constrain functions
////////////////////////////////////////////////////////////////////////
//
// Constrain functions take a space ('best_known_solution') that is has an assignment of variables
// and operate in the context of a fresh space, not yet fully assigned. Their purpose is to add
// constraints such that the assignments in the fresh space will either yield a better solution, or
// none at all if the best_known_solution is the best possible.
// 

#ifdef TOTAL_DISABLED_COST
//
// Very simple constraint function that only minimizes total disabled packages. This is left here
// for debugging purposes. Turn this on to test that the basic system can be solved.
//
void VersionProblem::constrain(const Space & _best_known_solution)
{
  const VersionProblem& best_known_solution = static_cast<const VersionProblem &>(_best_known_solution);

  // add first-level objective function minimization (failing packages, weighted)
  // new constraint: total_disabled < best_known_total_disabled_value)
  int best_known_total_disabled_value = best_known_solution.total_disabled.val();
  rel(*this, total_disabled, IRT_LE, best_known_total_disabled_value);
  PrintVarAligned("Constrain: total_disabled: ", total_disabled);
}
#endif // TOTAL_DISABLED_COST

#ifdef AGGREGATE_COST
//
// The aggregate cost function combines multiple cost functions as a linear combination to produce a single value
// The weightings are chosen so that a more important cost function is never outweighed by a change in the lower order functions
// This works, but suffers from problems with integer range; combining 3 cost functions with a range of 1000 requires 1E9 distinct values
// Since the number of packages drives the range of cost functions this puts a limit on the number of packages and number of cost functions.
// 
void VersionProblem::constrain(const Space & _best_known_solution)
{
  const VersionProblem& best_known_solution = static_cast<const VersionProblem &>(_best_known_solution);

  int best_known_aggregate_cost_value = best_known_solution.aggregate_cost.val();
  rel(*this, aggregate_cost, IRT_LE, best_known_aggregate_cost_value);
  PrintVarAligned("Constrain: best_known_aggregate_cost_value: ", best_known_aggregate_cost_value);
}
#endif // AGGREGATE_COST



// _best_known_soln is the most recent satisfying assignment of
// variables that Gecode has found. This method examines the solution
// and adds additional constraints that are applied after restarting
// the search, which means that the next time a solution that's found
// must be strictly better than the current best known solution.
//
// Our model requires us to have a series of objective functions where
// each successive objective function is evaluated if and only if all
// higher precedent objective functions are tied.
//
// [TODO: DESCRIBE WHAT THE ACTUAL SERIES OF OBJECTIVE FUNCTIONS IS]
//
// Lower precedent objective functions are modeled as the consequent
// of an implication whose antecedent is the conjunction of all the
// higher precedent objective functions being assigned to their best
// known value; thus, the optimal value of an objection function
// "activates" the next highest objective function. This has the
// effect of isolating the logic of each objective function such that
// it is only applied to the set of equally preferable solutions under
// the higher precedent objective functions. The objective function
// then applies its constraints, the solution space is restarted and
// walks the space until it finds another, more constrained solution.

#ifdef VECTOR_CONSTRAIN
// 
// The vector constrain function assembles multiple cost functions into a vector cost, and then
// constrains the vector cost to be less than the vector cost of the current best_known_solution.
// The less than operation here is a pairwise comparison in order of decreasing precedence; only if
// higher precedence elements are tied will the lower precedence elements be consulted. The elements 
// are in increasing order of precedence. 
//
// In this case the lowest precedence cost is total_not_preferred_at_latest, followed by total_preferred_at_latest
// and finally total_disabled.
//
void VersionProblem::constrain(const Space & _best_known_solution)
{
  const VersionProblem& best_known_solution = static_cast<const VersionProblem &>(_best_known_solution);

  IntVarArgs current(5);
  IntVarArgs best(5);
  current[0] = total_not_preferred_at_latest;
  current[1] = total_preferred_at_latest;
  current[2] = total_suspicious_disabled;
  current[3] = total_induced_disabled;
  current[4] = total_required_disabled;
  best[0] = best_known_solution.total_not_preferred_at_latest;
  best[1] = best_known_solution.total_preferred_at_latest;
  best[2] = best_known_solution.total_suspicious_disabled;
  best[3] = best_known_solution.total_induced_disabled;
  best[4] = best_known_solution.total_required_disabled;
  
  ConstrainVectorLessThanBest(current, best);
}
#endif // VECTOR_CONSTRAIN

IntVar & VersionProblem::GetPackageVersionVar(int packageId)
{
  if (packageId < cur_package) {
    return package_versions[packageId];
  } else {
#ifdef DEBUG
    std::cout << "Bad package Id " << packageId << " >= " << cur_package << std::endl;
    std::cout.flush();
#endif //DEBUG
    //    return 0;
  }
}

int VersionProblem::GetPackageVersion(int packageId) 
{
  IntVar & var = GetPackageVersionVar(packageId);
  if (1 == var.size()) return var.val();
  return UNRESOLVED_VARIABLE;
}
bool VersionProblem::GetPackageDisabledState(int packageId) 
{
  return disabled_package_variables[packageId].val() == 1;
}

int VersionProblem::GetMax(int packageId)
{
  return GetPackageVersionVar(packageId).max();
}
int VersionProblem::GetMin(int packageId)
{
  return GetPackageVersionVar(packageId).min();
}

int VersionProblem::GetDisabledVariableCount()
{
  if (total_disabled.min() == total_disabled.max()) {
    return total_disabled.min();
  } else {
    return UNRESOLVED_VARIABLE;
  }
}
  

// Utility
void VersionProblem::Print(std::ostream & out) 
{
  out << "Version problem dump:                   " << cur_package << "/" << size << " packages used/allocated" << std::endl; 
  out << "Disabled Variables:                     " << disabled_package_variables << std::endl;
  out << "Total Disabled variables (required):    " << total_required_disabled << std::endl;
  out << "Total Disabled variables: (induced):    " << total_induced_disabled << std::endl;
  out << "Total Disabled variables: (suspicious): " << total_suspicious_disabled << std::endl;
  out << "Total Disabled variables:               " << total_disabled << std::endl;
  out << "at_latest:                              " << at_latest << std::endl;
  out << "total_preferred_at_latest:              " << total_preferred_at_latest << std::endl;
  out << "total_not_preferred_at_latest:          " << total_not_preferred_at_latest << std::endl;
#ifdef COMPUTE_LINEAR_AGGREGATE
  out << "aggregate_cost:                         " << aggregate_cost << std::endl;
#endif // COMPUTE_LINEAR_AGGREGATE
  for (int i = 0; i < cur_package; i++) {
    out << "\t";
    PrintPackageVar(out, i);
    out << std::endl;
  }
  out.flush();
}

// TODO: Validate package ids !

void VersionProblem::PrintPackageVar(std::ostream & out, int packageId) 
{
  IntVar & var = GetPackageVersionVar(packageId);
  out << "PackageId: " << packageId <<  " Sltn: " << var << " disabled: " << disabled_package_variables[packageId] << " at latest: " << at_latest[packageId];
}

bool VersionProblem::CheckPackageId(int id) 
{
  return (id < size);
}

// We want to sort vectors 
// This constrains current to be less than best by a process analogous to subtraction
// we compute current - best, pairwise with borrows from less significant elements. We require it to be less than zero by requiring the most 
// significant element to generate a borrow. 
// 
void VersionProblem::ConstrainVectorLessThanBest(IntVarArgs & current, IntVarArgs & best) {
  BoolVarArray borrow(*this, current.size()+1, 0, 1);

  // No borrows can happen at the least significant element.
  rel(*this, borrow[0], IRT_EQ, 0);

  for (int i = 0; i < current.size(); i++) {
    // If best+borrow is greater than current (equivalently current-(best+borrow) is < 0) then a more significant element 
    // must have decreased, so we propagate a borrow to the next most significant element.
    int best_val = best[i].val();
    IntVar delta = expr(*this, current[i] - best_val - borrow[i]);
    // (delta < 0) <=> borrow[i+1]
    rel(*this, delta, IRT_LE, 0, borrow[i+1]);
#ifdef DEBUG
    std::cout << "ConstrainVector: borrow[" << i+1 << "] " << borrow[i+1] << ",\tdelta " << delta << std::endl;
    std::cout << "ConstrainVector: current[" << i << "] " << current[i] << ",\tbest_val " << best_val << std::endl;
#endif //DEBUG
  }

  // must borrow off past the most significant element.
  rel(*this, borrow[current.size()], IRT_EQ, 1);
}

VersionProblem * VersionProblem::Solve(VersionProblem * problem) 
{
  problem->Finalize();
  problem->status();
#ifdef DEBUG
  std::cout << "Before solve" << std::endl;
  problem->Print(std::cout);
#endif //DEBUG

  Restart<VersionProblem> solver(problem);
  int i = 0;
  VersionProblem *best_solution = NULL;
  while (VersionProblem *solution = solver.next())
    {
      if (best_solution != NULL) 
	{
	  delete best_solution;
	}
      best_solution = solution;
      ++i;
#ifdef DEBUG
      std::cout << "Trial Solution #" << i << "===============================" << std::endl;
      const Search::Statistics & stats = solver.statistics();
      std::cout << "Solver stats: Prop:" << stats.propagate << " Fail:" << stats.fail << " Node:" << stats.node;
      std::cout << " Depth:" << stats.depth << " memory:" << stats.memory << std::endl;
      solution->Print(std::cout);
#endif //DEBUG
    }

#ifdef DEBUG
  std::cout << "Solution completed: " << (best_solution ? "Found solution" : "No solution found") << std::endl;
  std::cout << "======================================================================" << std::endl;
  std::cout.flush();
#endif // DEBUG

  return best_solution;
}

//
// Debug output
// 
template <class T> void PrintVarAligned(const char * message, T & var) 
{
#ifdef DEBUG
  std::cout.width(40);
  std::cout << std::left << message << var << std::endl;
  std::cout.width(0);
#endif
}
template <class S, class T> void PrintVarAligned(const char * message, S & var1, T & var2) 
{
#ifdef DEBUG
  std::cout.width(40);
  std::cout << std::left << message << var1 << " " << var2 << std::endl;
  std::cout.width(0);
#endif
}

//template void PrintVarAligned<int>(const char * message, int & var);



//
// Version Problem
//
//
// 
//