/*
 * ProcessGroupWorker.cpp
 *
 *  Created on: Jun 24, 2014
 *      Author: heenemo
 */

#include "sgpp/distributedcombigrid/manager/ProcessGroupWorker.hpp"

#include "boost/lexical_cast.hpp"

#include "sgpp/distributedcombigrid/fullgrid/FullGrid.hpp"
#include "sgpp/distributedcombigrid/manager/CombiParameters.hpp"
#include "sgpp/distributedcombigrid/manager/ProcessGroupSignals.hpp"
#include "sgpp/distributedcombigrid/sparsegrid/DistributedSparseGrid.hpp"
#include "sgpp/distributedcombigrid/sparsegrid/DistributedSparseGridUniform.hpp"
#include "sgpp/distributedcombigrid/combicom/CombiCom.hpp"
#include "sgpp/distributedcombigrid/hierarchization/DistributedHierarchization.hpp"
#include "sgpp/distributedcombigrid/hierarchization/Hierarchization.hpp"
#include "sgpp/distributedcombigrid/mpi/MPIUtils.hpp"
#include <Python.h>
#include <boost/python.hpp>
#include <set>

namespace combigrid {

ProcessGroupWorker::ProcessGroupWorker() :
            currentTask_( NULL),
            status_(PROCESS_GROUP_WAIT),
            combinedFG_( NULL),
            combinedUniDSG_(NULL),
            combinedFGexists_(false),
            combiParameters_(),
            combiParametersSet_(false)
{
}

ProcessGroupWorker::~ProcessGroupWorker() {
  delete combinedFG_;
}

SignalType ProcessGroupWorker::wait() {
  if (status_ == PROCESS_GROUP_BUSY)
    return RUN_NEXT;

  SignalType signal = -1;

  MASTER_EXCLUSIVE_SECTION {
    // receive signal from manager
    MPI_Recv( &signal, 1, MPI_INT,
        theMPISystem()->getManagerRank(),
        signalTag,
        theMPISystem()->getGlobalComm(),
        MPI_STATUS_IGNORE);
  }
  // distribute signal to other processes of pgroup
  MPI_Bcast( &signal, 1, MPI_INT,
      theMPISystem()->getMasterRank(),
      theMPISystem()->getLocalComm() );
  // process signal
  if (signal == RUN_FIRST) {

    Task* t;

    // local root receives task
    MASTER_EXCLUSIVE_SECTION {
      Task::receive(  &t,
          theMPISystem()->getManagerRank(),
          theMPISystem()->getGlobalComm() );
    }

    // broadcast task to other process of pgroup
    Task::broadcast(&t, theMPISystem()->getMasterRank(), theMPISystem()->getLocalComm());

    MPI_Barrier(theMPISystem()->getLocalComm());

    // add task to task storage
    tasks_.push_back(t);

    status_ = PROCESS_GROUP_BUSY;

    // set currentTask
    currentTask_ = tasks_.back();

    // initalize task
    currentTask_->init(theMPISystem()->getLocalComm());

    // execute task
    currentTask_->run(theMPISystem()->getLocalComm());

  } else if (signal == RUN_NEXT) {
    // this should not happen
    assert(tasks_.size() > 0);

    // reset finished status of all tasks
    for (size_t i = 0; i < tasks_.size(); ++i)
      tasks_[i]->setFinished(false);

    status_ = PROCESS_GROUP_BUSY;

    // set currentTask
    currentTask_ = tasks_[0];

    // run first task
    currentTask_->run(theMPISystem()->getLocalComm());

  } else if (signal == ADD_TASK) {
    std::cout << "adding a single task" << std::endl;

    Task* t;

    // local root receives task
    MASTER_EXCLUSIVE_SECTION {
      Task::receive(&t, theMPISystem()->getManagerRank(), theMPISystem()->getGlobalComm());
    }

    // broadcast task to other process of pgroup
    Task::broadcast(&t, theMPISystem()->getMasterRank(), theMPISystem()->getLocalComm());

    MPI_Barrier(theMPISystem()->getLocalComm());

    // check if task already exists on this group
    for ( auto tmp : tasks_ )
      assert( tmp->getID() != t->getID() );

    // initalize task and set values to zero
    // the task will get the proper initial solution during the next combine
    t->init( theMPISystem()->getLocalComm() );

    t->setZero();

    t->setFinished( true );

    // add task to task storage
    tasks_.push_back(t);

    status_ = PROCESS_GROUP_BUSY;

  } else if (signal == EVAL) {
    // receive x

    // loop over all tasks
    // t.eval(x)
  } else if (signal == EXIT) {

  } else if (signal == SYNC_TASKS) {
    MASTER_EXCLUSIVE_SECTION {
      for (size_t i = 0; i < tasks_.size(); ++i) {
        Task::send(&tasks_[i], theMPISystem()->getManagerRank(), theMPISystem()->getGlobalComm());
      }
    }
  } else if (signal == COMBINE) {

    combineUniform();

  } else if (signal == GRID_EVAL) {

    gridEval();
    return signal;

  } else if (signal == COMBINE_FG) {

    combineFG();

  } else if (signal == UPDATE_COMBI_PARAMETERS) {

    updateCombiParameters();

  } else if (signal == RECOMPUTE) {
    Task* t;

    // local root receives task
    MASTER_EXCLUSIVE_SECTION {
      Task::receive(&t, theMPISystem()->getManagerRank(), theMPISystem()->getGlobalComm());
    }

    // broadcast task to other process of pgroup
    Task::broadcast(&t, theMPISystem()->getMasterRank(), theMPISystem()->getLocalComm());

    MPI_Barrier(theMPISystem()->getLocalComm());

    // add task to task storage
    tasks_.push_back(t);

    status_ = PROCESS_GROUP_BUSY;

    // set currentTask
    currentTask_ = tasks_.back();

    // initalize task
    currentTask_->init(theMPISystem()->getLocalComm());

    currentTask_->setZero();

    // fill task with combisolution
    setCombinedSolutionUniform( currentTask_ );

    // execute task
    currentTask_->run(theMPISystem()->getLocalComm());
    //  } else if ( signal ==  RECOVER_COMM ){
    //    theMPISystem()->recoverCommunicators( true );
    //    return signal;
  } else if (signal == SEARCH_SDC) {
    searchSDC();
  } else if (signal == REINIT_TASK) {
    std::cout << "reinitializing a single task" << std::endl;

    Task* t;

    // local root receives task
    MASTER_EXCLUSIVE_SECTION {
      Task::receive(&t, theMPISystem()->getManagerRank(), theMPISystem()->getGlobalComm());
    }

    // broadcast task to other process of pgroup
    Task::broadcast(&t, theMPISystem()->getMasterRank(), theMPISystem()->getLocalComm());

    MPI_Barrier(theMPISystem()->getLocalComm());

    for (auto tt : tasks_){
      if (tt->getID() == t->getID()){
        currentTask_ = tt;
        break;
      }
    }

    // initalize task and set values to zero
    currentTask_->init( theMPISystem()->getLocalComm() );
    currentTask_->setZero();
    setCombinedSolutionUniform( currentTask_ );
    currentTask_->setFinished(true);
  }

  // in the general case: send ready signal.
  if(!omitReadySignal)
    ready();

  return signal;
}

void ProcessGroupWorker::ready() {

  // check if there are unfinished tasks
  for (size_t i = 0; i < tasks_.size(); ++i) {
    if (!tasks_[i]->isFinished()) {
      status_ = PROCESS_GROUP_BUSY;

      // set currentTask
      currentTask_ = tasks_[i];
      currentTask_->run(theMPISystem()->getLocalComm());
    }
  }

  // all tasks finished -> group waiting
  if( status_ != PROCESS_GROUP_FAIL )
    status_ = PROCESS_GROUP_WAIT;

  MASTER_EXCLUSIVE_SECTION{
    StatusType status = status_;
    MPI_Send(&status, 1, MPI_INT, theMPISystem()->getManagerRank(), statusTag, theMPISystem()->getGlobalComm());
  }

  // reset current task
  currentTask_ = NULL;
}

void ProcessGroupWorker::combine() {
  // early exit if no tasks available
  // todo: doesnt work, each pgrouproot must call reduce function
  assert(tasks_.size() > 0);

  assert( combiParametersSet_ );
  DimType dim = combiParameters_.getDim();
  const LevelVector& lmin = combiParameters_.getLMin();
  const LevelVector& lmax = combiParameters_.getLMax();
  const std::vector<bool>& boundary = combiParameters_.getBoundary();

  // erzeug dsg
  DistributedSparseGrid<CombiDataType> dsg(dim, lmax, lmin, boundary, theMPISystem()->getLocalComm());

  for (Task* t : tasks_) {
    DistributedFullGrid<CombiDataType>& dfg = t->getDistributedFullGrid();

    // hierarchize dfg
    DistributedHierarchization::hierarchize<CombiDataType>(dfg);

    // lokales reduce auf sg ->
    //CombiCom::distributedLocalReduce<CombiDataType>( dfg, dsg, combiParameters_.getCoeff( t->getID() ) );
  }

  // globales reduce
  CombiCom::distributedGlobalReduce(dsg);

  for (Task* t : tasks_) {
    // get handle to dfg
    DistributedFullGrid<CombiDataType>& dfg = t->getDistributedFullGrid();

    // lokales scatter von dsg auf dfg
    //CombiCom::distributedLocalScatter<CombiDataType>( dfg, dsg );

    // dehierarchize dfg
    DistributedHierarchization::dehierarchize<CombiDataType>(dfg);

  }
}

void ProcessGroupWorker::combineUniform() {
  // early exit if no tasks available
  // todo: doesnt work, each pgrouproot must call reduce function
  assert(tasks_.size() > 0);

  assert( combiParametersSet_ );
  DimType dim = combiParameters_.getDim();
  const LevelVector& lmin = combiParameters_.getLMin();
  LevelVector lmax = combiParameters_.getLMax();
  const std::vector<bool>& boundary = combiParameters_.getBoundary();;

  for (size_t i = 0; i < lmax.size(); ++i)
    if (lmax[i] > 1)
      lmax[i] -= 1;

  // todo: delete old dsg
  if (combinedUniDSG_ != NULL)
    delete combinedUniDSG_;

  // erzeug dsg
  combinedUniDSG_ = new DistributedSparseGridUniform<CombiDataType>(dim, lmax,
      lmin, boundary,
      theMPISystem()->getLocalComm());

  // todo: move to init function to avoid reregistering
  // register dsg in all dfgs
  for (Task* t : tasks_) {
    DistributedFullGrid<CombiDataType>& dfg = t->getDistributedFullGrid();

    dfg.registerUniformSG(*combinedUniDSG_);
  }

  for (Task* t : tasks_) {

    DistributedFullGrid<CombiDataType>& dfg = t->getDistributedFullGrid();

    // hierarchize dfg
    DistributedHierarchization::hierarchize<CombiDataType>(dfg);

    // lokales reduce auf sg ->
    dfg.addToUniformSG( *combinedUniDSG_, combiParameters_.getCoeff( t->getID() ) );
  }

  CombiCom::distributedGlobalReduce( *combinedUniDSG_ );
  for (Task* t : tasks_) {
    // get handle to dfg
    DistributedFullGrid<CombiDataType>& dfg = t->getDistributedFullGrid();
    // extract dfg vom dsg
    dfg.extractFromUniformSG( *combinedUniDSG_ );

    // dehierarchize dfg
    DistributedHierarchization::dehierarchize<CombiDataType>( dfg );
  }

}

void ProcessGroupWorker::gridEval() {
  /* error if no tasks available
   * todo: however, this is not a real problem, we could can create an empty
   * grid an contribute to the reduce operation. at the moment even the dim
   * parameter is stored in the tasks, so if no task available we have no access
   * to this parameter.
   */
  assert(tasks_.size() > 0);

  assert(combiParametersSet_);
  const DimType dim = combiParameters_.getDim();

  LevelVector leval(dim);

  // receive leval
  MASTER_EXCLUSIVE_SECTION{
    // receive size of levelvector = dimensionality
    MPI_Status status;
    int bsize;
    MPI_Probe( theMPISystem()->getManagerRank(), 0, theMPISystem()->getGlobalComm(), &status);
    MPI_Get_count(&status, MPI_INT, &bsize);

    assert(bsize == static_cast<int>(dim));

    std::vector<int> tmp(dim);
    MPI_Recv( &tmp[0], bsize, MPI_INT,
        theMPISystem()->getManagerRank(), 0,
        theMPISystem()->getGlobalComm(), MPI_STATUS_IGNORE);
    leval = LevelVector(tmp.begin(), tmp.end());
  }

  assert( combiParametersSet_ );
  const std::vector<bool>& boundary = combiParameters_.getBoundary();
  FullGrid<CombiDataType> fg_red(dim, leval, boundary);

  // create the empty grid on only on localroot
  MASTER_EXCLUSIVE_SECTION {
    fg_red.createFullGrid();
  }

  // collect fg on pgrouproot and reduce
  for (size_t i = 0; i < tasks_.size(); ++i) {
    Task* t = tasks_[i];

    FullGrid<CombiDataType> fg(t->getDim(), t->getLevelVector(), boundary );

    MASTER_EXCLUSIVE_SECTION {
      fg.createFullGrid();
    }

    t->getFullGrid( fg,
        theMPISystem()->getMasterRank(),
        theMPISystem()->getLocalComm() );

    MASTER_EXCLUSIVE_SECTION{
      fg_red.add(fg, combiParameters_.getCoeff( t->getID() ) );
    }
  }
  // global reduce of f_red
  MASTER_EXCLUSIVE_SECTION {
    CombiCom::FGReduce( fg_red,
        theMPISystem()->getManagerRank(),
        theMPISystem()->getGlobalComm() );
  }
}

//todo: this is just a temporary function which will drop out some day
// also this function requires a modified fgreduce method which uses allreduce
// instead reduce in manger
void ProcessGroupWorker::combineFG() {
  //gridEval();

  // TODO: Sync back to fullgrids
}

void ProcessGroupWorker::updateCombiParameters() {
  CombiParameters tmp;

  // local root receives task
  MASTER_EXCLUSIVE_SECTION {
    MPIUtils::receiveClass(
        &tmp,
        theMPISystem()->getManagerRank(),
        theMPISystem()->getGlobalComm() );
  }

  // broadcast task to other process of pgroup
  MPIUtils::broadcastClass(
      &tmp,
      theMPISystem()->getMasterRank(),
      theMPISystem()->getLocalComm() );

  combiParameters_ = tmp;

  combiParametersSet_ = true;

  status_ = PROCESS_GROUP_BUSY;

}


void ProcessGroupWorker::setCombinedSolutionUniform( Task* t ) {
  assert( combinedUniDSG_ != NULL );

  // get handle to dfg
  DistributedFullGrid<CombiDataType>& dfg = t->getDistributedFullGrid();

  // extract dfg from dsg
  dfg.extractFromUniformSG( *combinedUniDSG_ );

  // dehierarchize dfg
  DistributedHierarchization::dehierarchize<CombiDataType>( dfg );
}

void ProcessGroupWorker::searchSDC(){
  SDCMethodType method = -1;
  MASTER_EXCLUSIVE_SECTION {
    // receive signal from manager
    MPI_Recv( &method, 1, MPI_INT,
        theMPISystem()->getManagerRank(),
        infoTag,
        theMPISystem()->getGlobalComm(),
        MPI_STATUS_IGNORE);
  }
  // distribute signal to other processes of pgroup
  MPI_Bcast( &method, 1, MPI_INT,
      theMPISystem()->getMasterRank(),
      theMPISystem()->getLocalComm() );

  std::vector<int> levelsSDC;
  if( method == COMPARE_PAIRS )
    comparePairsDistributed( 2, levelsSDC );
  if( method == COMPARE_VALUES )
    compareValues();

  int numLocalSDC = levelsSDC.size();
  int numGlobalSDC;
  MPI_Allreduce( &numLocalSDC, &numGlobalSDC, 1, MPI_INT, MPI_SUM, theMPISystem()->getLocalComm());
  if ( numGlobalSDC > 0 ){
    MPI_Status statusSDC;
    if( numLocalSDC > 0 ){
      if (!theMPISystem()->isMaster()){
        MPI_Send( &levelsSDC[0], numLocalSDC, MPI_INT, theMPISystem()->getMasterRank(), infoTag, theMPISystem()->getLocalComm() );
      }
    }else{
      MASTER_EXCLUSIVE_SECTION{
        levelsSDC.resize(tasks_.size());
        MPI_Recv( &levelsSDC[0], tasks_.size(), MPI_INT, MPI_ANY_SOURCE, infoTag, theMPISystem()->getLocalComm(), &statusSDC );
        MPI_Get_count( &statusSDC, MPI_INT, &numGlobalSDC );
        levelsSDC.resize(numGlobalSDC);
      }
    }
    MASTER_EXCLUSIVE_SECTION{
      status_ = PROCESS_GROUP_FAIL;
      MPI_Send( &levelsSDC[0], numGlobalSDC, MPI_INT, theMPISystem()->getManagerRank(), infoTag, theMPISystem()->getGlobalComm());
    }
  }
}

void ProcessGroupWorker::comparePairsDistributed( int numNearestNeighbors, std::vector<int> &levelsSDC ){

  DistributedSparseGridUniform<CombiDataType>* SDCUniDSG = new DistributedSparseGridUniform<CombiDataType>(
      combiParameters_.getDim(), combiParameters_.getLMax(), combiParameters_.getLMin(),
      combiParameters_.getBoundary(), theMPISystem()->getLocalComm());

  MPI_File_open(theMPISystem()->getLocalComm(), "out/all-betas-0.txt", MPI_MODE_CREATE|MPI_MODE_RDWR, MPI_INFO_NULL, &betasFile_ );

  for (auto t : tasks_){
    DistributedFullGrid<CombiDataType>& dfg = t->getDistributedFullGrid();
    DistributedHierarchization::hierarchize<CombiDataType>(dfg);
    dfg.registerUniformSG( *SDCUniDSG );
  }

  /* Generate all pairs of grids */
  std::vector<std::vector<Task*>> allPairs;

  generatePairs( numNearestNeighbors, allPairs );

  std::vector<CombiDataType> allBetas;
  std::vector<CombiDataType> allBetasSum;
  std::vector<LevelVector> allSubs;
  std::vector<size_t> allJs;
  for (auto pair : allPairs){

    DistributedFullGrid<CombiDataType>& dfg_t = pair[0]->getDistributedFullGrid();
    DistributedFullGrid<CombiDataType>& dfg_s = pair[1]->getDistributedFullGrid();

    dfg_t.addToUniformSG( *SDCUniDSG, 1.0 );
    dfg_s.addToUniformSG( *SDCUniDSG, -1.0 );

    CombiDataType localBetaMax(0.0);

    LevelVector subMax;
    size_t jMax = 0;

    for (size_t i = 0; i < SDCUniDSG->getNumSubspaces(); ++i){
      auto subData = SDCUniDSG->getData(i);
      auto subSize = SDCUniDSG->getDataSize(i);
      for (size_t j = 0; j < subSize; ++j){
        if (std::abs(subData[j]) > std::abs(localBetaMax)){
          localBetaMax = subData[j];
          subMax = SDCUniDSG->getLevelVector(i);
          jMax = j;
        }
      }
    }
    allBetas.push_back( localBetaMax );
    allSubs.push_back( subMax );
    allJs.push_back( jMax );

    // Remove from sparse grid
    dfg_t.addToUniformSG( *SDCUniDSG, -1.0 );
    dfg_s.addToUniformSG( *SDCUniDSG, 1.0 );
  }

  std::vector<CombiDataType> allBetasReduced;
  allBetasReduced.resize(allBetas.size());

  CombiCom::BetasReduce( allBetas, allBetasReduced, theMPISystem()->getLocalComm() );

  auto globalBetaMax = std::max_element(allBetasReduced.begin(), allBetasReduced.end(),
      [](CombiDataType a, CombiDataType b){ return std::abs(a) < std::abs(b); } );

  auto b = std::find( allBetas.begin(), allBetas.end(), *globalBetaMax );

  betas_.clear();


  if(b != allBetas.end()) {

    size_t indMax = std::distance(allBetas.begin(), b);

    LevelVector subMax = allSubs[indMax];
    std::cout<<"Subspace with SDC = "<<subMax<<std::endl;

    size_t jMax = allJs[indMax];

    for (auto pair : allPairs){

      DistributedFullGrid<CombiDataType>& dfg_t = pair[0]->getDistributedFullGrid();
      DistributedFullGrid<CombiDataType>& dfg_s = pair[1]->getDistributedFullGrid();

      LevelVector t_level = pair[0]->getLevelVector();
      LevelVector s_level = pair[1]->getLevelVector();

      dfg_t.addToUniformSG( *SDCUniDSG, 1.0 );
      dfg_s.addToUniformSG( *SDCUniDSG, -1.0 );

      auto subData = SDCUniDSG->getData(subMax);
      CombiDataType localBetaMax = subData[jMax];
      betas_[std::make_pair(t_level, s_level)] = localBetaMax;

      dfg_t.addToUniformSG( *SDCUniDSG, -1.0 );
      dfg_s.addToUniformSG( *SDCUniDSG, 1.0 );
    }

//    filterSDCPython( levelsSDC );
    filterSDCGSL( levelsSDC );

  }
  MPI_Barrier(theMPISystem()->getLocalComm());
  for (auto t : tasks_){
    DistributedFullGrid<CombiDataType>& dfg = t->getDistributedFullGrid();
    DistributedHierarchization::dehierarchize<CombiDataType>(dfg);
  }
}
void ProcessGroupWorker::comparePairsSerial( int numNearestNeighbors, std::vector<int> &levelsSDC ){

  /* Generate all pairs of grids */
  std::vector<std::vector<Task*>> allPairs;

  generatePairs( numNearestNeighbors, allPairs );

  std::vector<CombiDataType> allBetas;
  std::vector<size_t> allJs;
  size_t jMax;

  //  MPI_File_open(theMPISystem()->getLocalComm(), "out/all-betas-0.txt", MPI_MODE_CREATE|MPI_MODE_RDWR, MPI_INFO_NULL, &betasFile_ );
  const DimType dim = combiParameters_.getDim();
  LevelVector lmax = combiParameters_.getLMax();
  const std::vector<bool>& boundary = combiParameters_.getBoundary();

  for (auto pair : allPairs){

    FullGrid<CombiDataType> fg_red(dim, lmax, boundary);
    FullGrid<CombiDataType> fg_t(dim, pair[0]->getLevelVector(), boundary );
    FullGrid<CombiDataType> fg_s(dim, pair[1]->getLevelVector(), boundary );
    CombiDataType localBetaMax(0.0);
    jMax = 0;

    // create the empty grid on only on localroot
    MASTER_EXCLUSIVE_SECTION {
      fg_red.createFullGrid();
      fg_t.createFullGrid();
      fg_s.createFullGrid();
    }

    pair[0]->getFullGrid( fg_t, theMPISystem()->getMasterRank(), theMPISystem()->getLocalComm() );
    pair[1]->getFullGrid( fg_s, theMPISystem()->getMasterRank(), theMPISystem()->getLocalComm() );

    MPI_Barrier(theMPISystem()->getLocalComm());
    MASTER_EXCLUSIVE_SECTION{
      fg_red.add(fg_t, 1.0 );
      fg_red.add(fg_s, -1.0 );
      Hierarchization::hierarchize<CombiDataType>(fg_red);
      auto data_red = fg_red.getData();
      for (size_t j = 0; j < fg_red.getNrElements(); ++j){
        if (std::abs(data_red[j]) > std::abs(localBetaMax)){
          localBetaMax = data_red[j];
          jMax = j;
        }
      }
      allBetas.push_back( localBetaMax );
      allJs.push_back( jMax );
    }
  }

  MASTER_EXCLUSIVE_SECTION{
    auto globalBetaMax = std::max_element(allBetas.begin(), allBetas.end(),
        [](CombiDataType a, CombiDataType b){ return std::abs(a) < std::abs(b); } );

    auto b = std::find( allBetas.begin(), allBetas.end(), *globalBetaMax );
    size_t indMax = std::distance(allBetas.begin(), b);
    jMax = allJs[indMax];

    FullGrid<CombiDataType> fg_red(dim, lmax, boundary);
    // create the empty grid on only on localroot
    fg_red.createFullGrid();
    LevelVector sdcLevel(dim);
    IndexVector indexes(dim);
    fg_red.getLI(jMax, sdcLevel,indexes);
    std::cout<<"SDC Level = " << sdcLevel<<", jMax = "<<jMax << ", gBMax = "<< *globalBetaMax<<std::endl;
    fg_red.deleteFullGrid();
  }
  for (auto pair : allPairs){

    FullGrid<CombiDataType> fg_red(dim, lmax, boundary);
    MASTER_EXCLUSIVE_SECTION{
      // create the empty grid on only on localroot
      fg_red.createFullGrid();
    }

    FullGrid<CombiDataType> fg_t(pair[0]->getDim(), pair[0]->getLevelVector(), boundary );
    FullGrid<CombiDataType> fg_s(pair[1]->getDim(), pair[1]->getLevelVector(), boundary );

    LevelVector t_level = pair[0]->getLevelVector();
    LevelVector s_level = pair[1]->getLevelVector();

    MASTER_EXCLUSIVE_SECTION{
      fg_t.createFullGrid();
      fg_s.createFullGrid();
    }

    pair[0]->getFullGrid( fg_t, theMPISystem()->getMasterRank(), theMPISystem()->getLocalComm() );
    pair[1]->getFullGrid( fg_s, theMPISystem()->getMasterRank(), theMPISystem()->getLocalComm() );

    MASTER_EXCLUSIVE_SECTION{
      fg_red.add(fg_t, 1.0 );
      fg_red.add(fg_s, -1.0 );
      Hierarchization::hierarchize<CombiDataType>(fg_red);
      auto data = fg_red.getData();
      CombiDataType localBetaMax = data[jMax];

      betas_[std::make_pair(t_level, s_level)] = localBetaMax;
    }
  }
  MASTER_EXCLUSIVE_SECTION{
    filterSDCPython( levelsSDC );
  }
}
int ProcessGroupWorker::compareValues(){
}

void ProcessGroupWorker::computeLMSResiduals( gsl_multifit_robust_workspace* regressionWsp, gsl_vector* r_lms ){

  size_t p = regressionWsp->p;
  size_t n = regressionWsp->n;
  gsl_vector *r = gsl_vector_alloc( n );
  gsl_vector *r2 = gsl_vector_alloc( n );
  gsl_vector *r2_sorted = gsl_vector_alloc( n );
  gsl_vector *r_stand = gsl_vector_alloc( n );
  gsl_vector *weights = gsl_vector_alloc( n );
  gsl_vector *ones = gsl_vector_alloc( n );

  gsl_multifit_robust_stats regressionStats = gsl_multifit_robust_statistics( regressionWsp );
  gsl_vector_memcpy(r, regressionStats.r);
  gsl_vector_memcpy(r_stand, regressionStats.r);
  gsl_vector_memcpy(r_lms, regressionStats.r);

  for (size_t i = 0; i < r->size; ++i)
    gsl_vector_set(r2, i, std::pow(gsl_vector_get(r, i),2));

  std::cout<<"Residuals2:\n";
  for(size_t i = 0; i < regressionStats.r->size; ++i)
    std::cout<<r2->data[i]<<" ";

  gsl_vector_memcpy(r2_sorted, r2);
  gsl_sort_vector(r2_sorted);

  double median_r2 = gsl_stats_median_from_sorted_data(r2_sorted->data, r2_sorted->stride, r2_sorted->size);

  std::cout<<"median r2 = "<<median_r2<<std::endl;

  // Preliminary scale estimate
  double s0 = 1.4826*( 1 + 5.0/(n-p-1))*(std::sqrt(median_r2));

  std::cout<<"s0 ="<<s0<<std::endl;
  // Standardized residuals
  gsl_vector_scale(r_stand, 1.0/s0);

  // Threshold for residuals
  double eps = 2.5;
  for(size_t i = 0; i < r_stand->size; ++i){
    if(std::abs(r_stand->data[i]) <= eps)
      gsl_vector_set(weights, i, 1);
    else
      gsl_vector_set(weights, i, 0);
  }
  std::cout<<"Stand Residuals:\n";
  for(size_t i = 0; i < regressionStats.r->size; ++i)
    std::cout<<r_stand->data[i]<<" ";
  // Robust scale estimate
  double prod;
  gsl_blas_ddot( weights, r2, &prod );
  std::cout<<"Prod = "<<prod<<std::endl;

  double sum;
  gsl_vector_set_all( ones, 1 );
  gsl_blas_ddot( weights, ones, &sum );
  std::cout<<"Sum = "<<sum<<std::endl;
  double s_star = std::sqrt(prod/(sum-p));

  std::cout<<"Residuals:\n";
  for(size_t i = 0; i < regressionStats.r->size; ++i)
    std::cout<<regressionStats.r->data[i]<<" ";
  std::cout<<"s_star = "<<s_star<<std::endl;
  gsl_vector_scale( r_lms, 1.0/s_star );
  std::cout<<"LMS Residuals:\n";
  for(size_t i = 0; i < regressionStats.r->size; ++i)
    std::cout<<r_lms->data[i]<<" ";
}

void ProcessGroupWorker::generatePairs( int numNearestNeighbors, std::vector<std::vector<Task*>> &allPairs ){

  std::vector<LevelVector> levels;
  std::map<LevelVector, int> numPairs;

  for ( auto tt: tasks_ ){
    levels.push_back(tt->getLevelVector());
    numPairs[tt->getLevelVector()] = 0;
  }

  for (Task* s : tasks_ ){

    std::sort(levels.begin(), levels.end(), [s](LevelVector const& a, LevelVector const& b) {
      return l1(a - s->getLevelVector()) < l1(b - s->getLevelVector());
    });

    int k = 0;

    for( size_t t_i = 1; t_i < levels.size(); ++t_i ){
      std::vector<Task*> currentPair;

      Task* t = *std::find_if(tasks_.begin(), tasks_.end(),
          [levels,t_i](Task* const &tt) -> bool { return tt->getLevelVector() == levels[t_i]; });

      currentPair.push_back(t);
      currentPair.push_back(s);

      if(std::find(allPairs.begin(), allPairs.end(), currentPair) == allPairs.end()
          //          && numPairs[s->getLevelVector()] < numNearestNeighbors
          //          && numPairs[t->getLevelVector()] < numNearestNeighbors
      ){
        allPairs.push_back({currentPair[1],currentPair[0]});
        numPairs[s->getLevelVector()]++;
        numPairs[t->getLevelVector()]++;
        k++;
      }

      if (k == numNearestNeighbors)
        break;
    }
  }
  // Check if any grid was left out with fewer neighbors than it should
  for (Task* s : tasks_ ){

    int k = numPairs[s->getLevelVector()];
    if ( k < numNearestNeighbors ){

      std::sort(levels.begin(), levels.end(), [s](LevelVector const& a, LevelVector const& b) {
        return l1(a - s->getLevelVector()) < l1(b - s->getLevelVector());
      });


      for( size_t t_i = 1; t_i < levels.size(); ++t_i ){
        std::vector<Task*> currentPair;
        std::vector<Task*> currentPairBack;

        Task* t = *std::find_if(tasks_.begin(), tasks_.end(),
            [levels,t_i](Task* const &tt) -> bool { return tt->getLevelVector() == levels[t_i]; });

        currentPair.push_back(s);
        currentPair.push_back(t);
        currentPairBack.push_back(t);
        currentPairBack.push_back(s);

        if(std::find(allPairs.begin(), allPairs.end(), currentPair) == allPairs.end() &&
            std::find(allPairs.begin(), allPairs.end(), currentPairBack) == allPairs.end()){
          allPairs.push_back({currentPair[1],currentPair[0]});
          numPairs[s->getLevelVector()]++;
          numPairs[t->getLevelVector()]++;
          k++;
        }

        if (k == numNearestNeighbors)
          break;
      }
    }
  }
}

void ProcessGroupWorker::filterSDCGSL( std::vector<int> &levelsSDC ){

  // Number of measurements (beta values)
  size_t n = betas_.size();

  auto lmin = combiParameters_.getLMin();
  auto lmax = combiParameters_.getLMax();
  size_t diff = lmax[0] - lmin[0] + 1;

  // Number of unknowns (functions D1, D2, and D12)
  size_t p = 2*diff;
  double ex = 2;

  if ( n < p )
    return;

  gsl_multifit_robust_workspace *regressionWsp = gsl_multifit_robust_alloc(gsl_multifit_robust_default, n , p );

//  double tune_const = 6.0;
//  gsl_multifit_robust_tune( tune_const, regressionWsp );

  //  gsl_multifit_robust_maxiter( 100, regressionWsp );

  gsl_matrix *X = gsl_matrix_alloc( n, p );
  gsl_vector *y = gsl_vector_alloc( n );
  gsl_vector *c = gsl_vector_alloc( p );
  gsl_vector *r_lms = gsl_vector_alloc( n );
  gsl_matrix *cov = gsl_matrix_alloc( p, p );

  // Initialize matrix with zeros
  gsl_matrix_set_zero( X );

  IndexType idx_D1, idx_D2;

  int row  = 0;
  CombiDataType val;
  for( auto const &entry : betas_ ){
    LevelVector key_t = entry.first.first;
    LevelVector key_s = entry.first.second;
    CombiDataType beta = entry.second;

    std::vector<CombiDataType> ht = {1.0/pow(2.0,key_t[0]), 1.0/pow(2.0,key_t[1])};
    std::vector<CombiDataType> hs = {1.0/pow(2.0,key_s[0]), 1.0/pow(2.0,key_s[1])};
    for( size_t i = 0; i < key_t.size(); ++i ){
      key_t[i] -= lmin[0];
      key_s[i] -= lmin[0];
    }

    idx_D1 =  key_t[0];
    gsl_matrix_set( X, row, idx_D1, pow(ht[0],ex) );

    idx_D1 =  key_s[0];
    val = gsl_matrix_get( X, row, idx_D1 );
    gsl_matrix_set( X, row, idx_D1, val - pow(hs[0],ex) );

    idx_D2 = diff + key_t[1];
    gsl_matrix_set( X, row, idx_D2, pow(ht[1],ex) );

    idx_D2 = diff + key_s[1];
    val = gsl_matrix_get( X, row, idx_D2 );
    gsl_matrix_set( X, row, idx_D2, val - pow(hs[1],ex) );

    gsl_vector_set( y, row, beta );

    row++;
  }

  gsl_set_error_handler_off();
  gsl_multifit_robust( X, y, c, cov, regressionWsp );

  gsl_multifit_robust_stats regressionStats = gsl_multifit_robust_statistics( regressionWsp );

  computeLMSResiduals( regressionWsp, r_lms );

  std::map<LevelVector,int> mapSDC;
  for ( auto const &entry : betas_ ){
    LevelVector key_t = entry.first.first;
    LevelVector key_s = entry.first.second;
    mapSDC[key_t] = 0;
    mapSDC[key_s] = 0;
  }

  row = 0;
  int numSDCPairs = 0;
  for( auto const &entry : betas_ ){
    LevelVector key_t = entry.first.first;
    LevelVector key_s = entry.first.second;
    CombiDataType beta = entry.second;
    if ( std::abs(r_lms->data[row]) > 2.5 && beta != 0 ){
      mapSDC[key_t]++;
      mapSDC[key_s]++;
      numSDCPairs++;
    }
    row++;
  }

  std::cout<< "SDC grid: " << std::endl;
  for (auto s : mapSDC){
    if (s.second >= 2 || (s.second == 1 && numSDCPairs == 1)){
      std::cout<<s.first<<std::endl;
      int id = combiParameters_.getID(s.first);
      levelsSDC.push_back(id);
    }
  }

  // Write pairs and their beta values to file
  std::stringstream buf;
  buf<<n<< std::endl;
  row = 0;
  for ( auto const &entry : betas_ ){
    LevelVector key_t = entry.first.first;
    LevelVector key_s = entry.first.second;
    CombiDataType beta = entry.second;
    CombiDataType res = r_lms->data[row];
    buf<<key_t <<","<< key_s <<","<<beta<<","<< res <<std::endl;
    MPI_File_seek(betasFile_, 0, MPI_SEEK_END);
    MPI_File_write(betasFile_, buf.str().c_str(), buf.str().size(), MPI_CHAR, MPI_STATUS_IGNORE);
    std::cout<<buf.str();
    buf.str("");
    row++;
  }
  // Write regression coefficients to file
  buf << c->size << std::endl;
  for( size_t i = 0; i < c->size; ++i){
    buf << c->data[i]<<std::endl;
    MPI_File_seek(betasFile_, 0, MPI_SEEK_END);
    MPI_File_write(betasFile_, buf.str().c_str(), buf.str().size(), MPI_CHAR, MPI_STATUS_IGNORE);
    buf.str("");
  }

  gsl_matrix_free(X);
  gsl_matrix_free(cov);
  gsl_vector_free(y);
  gsl_vector_free(c);
  gsl_multifit_robust_free(regressionWsp);
}

void ProcessGroupWorker::filterSDCPython( std::vector<int> &levelsSDC ){

  // Number of measurements (beta values)
  size_t n = betas_.size();

  auto lmin = combiParameters_.getLMin();
  auto lmax = combiParameters_.getLMax();
  size_t diff = lmax[0] - lmin[0] + 1;

  // Number of unknowns (functions D1, D2, and D12)
  size_t p = 2*diff;

  if ( n < p )
    return;

  namespace py = boost::python;

  // Cal Python routines
  Py_Initialize();
  py::object main_module = py::import("__main__");
  py::dict main_namespace = py::extract<py::dict>(main_module.attr("__dict__"));
  main_namespace["lmin"] = lmin[0];
  main_namespace["lmax"] = lmax[0];
  py::exec("t_train = [] \n"
      "y_train = []",main_namespace);
  py::dict dictionary;
  for( auto const &entry : betas_ ){
    py::list l1,l2;
    for(auto v: entry.first.first)
      l1.append(v);
    for(auto v: entry.first.second)
      l2.append(v);
    main_namespace["l1"] = l1;
    main_namespace["l2"] = l2;
    main_namespace["beta"] = entry.second;
    py::exec("t_train.append((tuple(l1),tuple(l2))) \n"
        "y_train.append(beta)", main_namespace);
  }
  try{
    py::object result = py::exec_file("outlier.py", main_namespace);
  }
  catch (py::error_already_set) {
    PyErr_Print();
  }
  // Obtain standardized LMS residuals
  py::object r_stand_lms = main_namespace["r_stand_lms"];
  std::vector<CombiDataType> stand_residuals;
  for(size_t i = 0; i < n; ++i)
    stand_residuals.push_back(py::extract<CombiDataType>(r_stand_lms[i]));

  // Look for outliers
  std::map<LevelVector,int> mapSDC;
  for ( auto const &entry : betas_ ){
    LevelVector key_t = entry.first.first;
    LevelVector key_s = entry.first.second;
    mapSDC[key_t] = 0;
    mapSDC[key_s] = 0;
  }
  size_t row = 0;
  size_t numSDCPairs = 0;
  double eps = 2.5;
  for( auto const &entry : betas_ ){
    LevelVector key_t = entry.first.first;
    LevelVector key_s = entry.first.second;
    CombiDataType beta = entry.second;
    if ( std::abs(stand_residuals[row]) > eps && beta != 0 ){
      mapSDC[key_t]++;
      mapSDC[key_s]++;
      numSDCPairs++;
    }
    row++;
  }
  std::cout<< "SDC grid: " << std::endl;
  for (auto s : mapSDC){
    if (s.second >= 2 || (s.second == 1 && numSDCPairs == 1)){
      std::cout<<s.first<<std::endl;
      int id = combiParameters_.getID(s.first);
      levelsSDC.push_back(id);
    }
  }
}
} /* namespace combigrid */
