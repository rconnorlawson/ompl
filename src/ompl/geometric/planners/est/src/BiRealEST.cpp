/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2015, Rice University
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Rice University nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Author: Ryan Luna */

#include "ompl/geometric/planners/est/BiRealEST.h"
#include "ompl/base/goals/GoalSampleableRegion.h"
#include "ompl/tools/config/SelfConfig.h"
#include <limits>
#include <cassert>

ompl::geometric::BiRealEST::BiRealEST(const base::SpaceInformationPtr &si) : base::Planner(si, "BiRealEST")
{
    specs_.recognizedGoal = base::GOAL_SAMPLEABLE_REGION;
    specs_.directed = true;
    maxDistance_ = 0.0;
    connectionPoint_ = std::make_pair<ompl::base::State*, ompl::base::State*>(NULL, NULL);

    Planner::declareParam<double>("range", this, &BiRealEST::setRange, &BiRealEST::getRange, "0.:1.:10000.");
}

ompl::geometric::BiRealEST::~BiRealEST()
{
    freeMemory();
}

void ompl::geometric::BiRealEST::setup()
{
    Planner::setup();

    if (maxDistance_ < 1e-3)
    {
        tools::SelfConfig sc(si_, getName());
        sc.configurePlannerRange(maxDistance_);

        // Make the neighborhood radius smaller than sampling range to
        // keep probabilities relatively high for rejection sampling
        nbrhoodRadius_ = maxDistance_ / 3.0;
    }

    if (!nnStart_)
        nnStart_.reset(tools::SelfConfig::getDefaultNearestNeighbors<Motion*>(this));
    if (!nnGoal_)
        nnGoal_.reset(tools::SelfConfig::getDefaultNearestNeighbors<Motion*>(this));
    nnStart_->setDistanceFunction(boost::bind(&BiRealEST::distanceFunction, this, _1, _2));
    nnGoal_->setDistanceFunction(boost::bind(&BiRealEST::distanceFunction, this, _1, _2));
}

void ompl::geometric::BiRealEST::clear()
{
    Planner::clear();
    sampler_.reset();
    freeMemory();
    if (nnStart_)
        nnStart_->clear();
    if (nnGoal_)
        nnGoal_->clear();

    startMotions_.clear();
    startPdf_.clear();

    goalMotions_.clear();
    goalPdf_.clear();

    connectionPoint_ = std::make_pair<base::State*, base::State*>(NULL, NULL);
}

void ompl::geometric::BiRealEST::freeMemory()
{
    for(size_t i = 0; i < startMotions_.size(); ++i)
    {
        if (startMotions_[i]->state)
            si_->freeState(startMotions_[i]->state);
        delete startMotions_[i];
    }

    for(size_t i = 0; i < goalMotions_.size(); ++i)
    {
        if (goalMotions_[i]->state)
            si_->freeState(goalMotions_[i]->state);
        delete goalMotions_[i];
    }
}

ompl::base::PlannerStatus ompl::geometric::BiRealEST::solve(const base::PlannerTerminationCondition &ptc)
{
    checkValidity();
    base::GoalSampleableRegion *goal = dynamic_cast<base::GoalSampleableRegion*>(pdef_->getGoal().get());

    if (!goal)
    {
        OMPL_ERROR("%s: Unknown type of goal", getName().c_str());
        return base::PlannerStatus::UNRECOGNIZED_GOAL_TYPE;
    }

    std::vector<Motion*> neighbors;

    while (const base::State *st = pis_.nextStart())
    {
        Motion *motion = new Motion(si_);
        si_->copyState(motion->state, st);
        motion->root = motion->state;

        nnStart_->nearestR(motion, nbrhoodRadius_, neighbors);
        addMotion(motion, startMotions_, startPdf_, nnStart_, neighbors);
    }

    if (startMotions_.size() == 0)
    {
        OMPL_ERROR("%s: There are no valid initial states!", getName().c_str());
        return base::PlannerStatus::INVALID_START;
    }

    if (!goal->couldSample())
    {
        OMPL_ERROR("%s: Insufficient states in sampleable goal region", getName().c_str());
        return base::PlannerStatus::INVALID_GOAL;
    }

    if (!sampler_)
        sampler_ = si_->allocValidStateSampler();

    OMPL_INFORM("%s: Starting planning with %u states already in datastructure", getName().c_str(), startMotions_.size() + goalMotions_.size());

    base::State *xstate = si_->allocState();
    Motion* xmotion = new Motion();

    bool startTree = true;
    bool solved = false;

    while (ptc == false && !solved)
    {
        // Make sure goal tree has at least one state.
        if (goalMotions_.size() == 0 || pis_.getSampledGoalsCount() < goalMotions_.size() / 2)
        {
            const base::State *st = goalMotions_.size() == 0 ? pis_.nextGoal(ptc) : pis_.nextGoal();
            if (st)
            {
                Motion *motion = new Motion(si_);
                si_->copyState(motion->state, st);
                motion->root = motion->state;

                nnGoal_->nearestR(motion, nbrhoodRadius_, neighbors);
                addMotion(motion, goalMotions_, goalPdf_, nnGoal_, neighbors);
            }

            if (goalMotions_.size() == 0)
            {
                OMPL_ERROR("%s: Unable to sample any valid states for goal tree", getName().c_str());
                break;
            }
        }

        // Pointers to the tree structure we are expanding
        std::vector<Motion*>& motions                       = startTree ? startMotions_ : goalMotions_;
        PDF<Motion*>& pdf                                   = startTree ? startPdf_     : goalPdf_;
        boost::shared_ptr< NearestNeighbors<Motion*> > nn   = startTree ? nnStart_      : nnGoal_;

        // Select a state to expand from
        Motion *existing = pdf.sample(rng_.uniform01());
        assert(existing);

        // Sample a state in the neighborhood
        if (!sampler_->sampleNear(xstate, existing->state, maxDistance_))
            continue;

        // Compute neighborhood of candidate state
        xmotion->state = xstate;
        nn->nearestR(xmotion, nbrhoodRadius_, neighbors);

        // reject state with probability proportional to neighborhood density
        if (neighbors.size())
        {
            double p = 1.0 - (1.0 / neighbors.size());
            if (rng_.uniform01() < p)
                continue;
        }

        // Is motion good?
        if (si_->checkMotion(existing->state, xstate))
        {
            // create a motion
            Motion *motion = new Motion(si_);
            si_->copyState(motion->state, xstate);
            motion->parent = existing;
            motion->root = existing->root;

            // add it to everything
            addMotion(motion, motions, pdf, nn, neighbors);

            // try to connect this state to the other tree
            // Get all states in the other tree within a maxDistance_ ball (bigger than "neighborhood" ball)
            startTree ? nnGoal_->nearestR(motion, maxDistance_, neighbors) : nnStart_->nearestR(motion, maxDistance_, neighbors);
            for(size_t i = 0; i < neighbors.size() && !solved; ++i)
            {
                if (goal->isStartGoalPairValid(motion->root, neighbors[i]->root) &&
                    si_->checkMotion(motion->state, neighbors[i]->state)) // win!  solution found.
                {
                    connectionPoint_ = std::make_pair(motion->state, neighbors[i]->state);

                    Motion* startMotion = startTree ? motion : neighbors[i];
                    Motion* goalMotion  = startTree ? neighbors[i] : motion;

                    Motion *solution = startMotion;
                    std::vector<Motion*> mpath1;
                    while (solution != NULL)
                    {
                        mpath1.push_back(solution);
                        solution = solution->parent;
                    }

                    solution = goalMotion;
                    std::vector<Motion*> mpath2;
                    while (solution != NULL)
                    {
                        mpath2.push_back(solution);
                        solution = solution->parent;
                    }

                    PathGeometric *path = new PathGeometric(si_);
                    path->getStates().reserve(mpath1.size() + mpath2.size());
                    for (int i = mpath1.size() - 1 ; i >= 0 ; --i)
                        path->append(mpath1[i]->state);
                    for (unsigned int i = 0 ; i < mpath2.size() ; ++i)
                        path->append(mpath2[i]->state);

                    pdef_->addSolutionPath(base::PathPtr(path), false, 0.0, getName());
                    solved = true;
                }
            }
        }

        // swap trees for next iteration
        startTree = !startTree;
    }

    si_->freeState(xstate);
    delete xmotion;

    OMPL_INFORM("%s: Created %u states (%u start + %u goal)", getName().c_str(), startMotions_.size() + goalMotions_.size(), startMotions_.size(), goalMotions_.size());
    return solved ? base::PlannerStatus::EXACT_SOLUTION : base::PlannerStatus::TIMEOUT;
}

void ompl::geometric::BiRealEST::addMotion(Motion* motion, std::vector<Motion*>& motions,
                                           PDF<Motion*>& pdf, boost::shared_ptr< NearestNeighbors<Motion*> > nn,
                                           const std::vector<Motion*>& neighbors)
{
    // Updating neighborhood size counts
    for(size_t i = 0; i < neighbors.size(); ++i)
    {
        PDF<Motion*>::Element *elem = neighbors[i]->element;
        double w = pdf.getWeight(elem);
        pdf.update(elem, w / (w + 1.));
    }

    motion->element = pdf.add(motion, 1. / (neighbors.size() + 1.));  // +1 for self
    motions.push_back(motion);
    nn->add(motion);
}

void ompl::geometric::BiRealEST::getPlannerData(base::PlannerData &data) const
{
    Planner::getPlannerData(data);

    for (unsigned int i = 0 ; i < startMotions_.size() ; ++i)
    {
        if (startMotions_[i]->parent == NULL)
            data.addStartVertex(base::PlannerDataVertex(startMotions_[i]->state, 1));
        else
            data.addEdge(base::PlannerDataVertex(startMotions_[i]->parent->state, 1),
                         base::PlannerDataVertex(startMotions_[i]->state, 1));
    }

    for (unsigned int i = 0 ; i < goalMotions_.size() ; ++i)
    {
        if (goalMotions_[i]->parent == NULL)
            data.addGoalVertex(base::PlannerDataVertex(goalMotions_[i]->state, 2));
        else
            // The edges in the goal tree are reversed to be consistent with start tree
            data.addEdge(base::PlannerDataVertex(goalMotions_[i]->state, 2),
                         base::PlannerDataVertex(goalMotions_[i]->parent->state, 2));
    }

    // Add the edge connecting the two trees
    data.addEdge(data.vertexIndex(connectionPoint_.first), data.vertexIndex(connectionPoint_.second));
}