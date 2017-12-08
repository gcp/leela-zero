/*
    This file is part of Leela Zero.
    Copyright (C) 2017 Gian-Carlo Pascutto

    Leela Zero is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Leela Zero is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Leela Zero.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <limits>
#include <cmath>

#include <iostream>
#include <vector>
#include <functional>
#include <algorithm>
#include <random>
#include <numeric>
#include "FastState.h"
#include "UCTNode.h"
#include "UCTSearch.h"
#include "Utils.h"
#include "Network.h"
#include "GTP.h"
#include "Random.h"
#ifdef USE_OPENCL
#include "OpenCL.h"
#endif

using namespace Utils;

UCTNode::UCTNode(int vertex, float score, float init_eval)
    : m_move(vertex), m_score(score), m_init_eval(init_eval) {
}

UCTNode::~UCTNode() {
    LOCK(get_mutex(), lock);
    for (auto& child : children) {
        delete child;
    }
}


bool UCTNode::first_visit() const {
    return m_visits == 0;
}

SMP::Mutex & UCTNode::get_mutex() {
    return m_nodemutex;
}

bool UCTNode::create_children(std::atomic<int> & nodecount,
                              GameState & state,
                              float & eval) {
    // check whether somebody beat us to it (atomic)
    if (has_children()) {
        return false;
    }
    // acquire the lock
    LOCK(get_mutex(), lock);
    // no successors in final state
    if (state.get_passes() >= 2) {
        return false;
    }
    // check whether somebody beat us to it (after taking the lock)
    if (has_children()) {
        return false;
    }
    // Someone else is running the expansion
    if (m_is_expanding) {
        return false;
    }
    // We'll be the one queueing this node for expansion, stop others
    m_is_expanding = true;
    lock.unlock();

    auto raw_netlist = Network::get_scored_moves(
        &state, Network::Ensemble::RANDOM_ROTATION);

    // DCNN returns winrate as side to move
    auto net_eval = raw_netlist.second;
    auto to_move = state.board.get_to_move();
    // our search functions evaluate from black's point of view
    if (to_move == FastBoard::WHITE) {
        net_eval = 1.0f - net_eval;
    }
    eval = net_eval;

    FastBoard & board = state.board;
    std::vector<Network::scored_node> nodelist;

    auto legal_sum = 0.0f;
    for (const auto& node : raw_netlist.first) {
        auto vertex = node.second;
        if (vertex != FastBoard::PASS) {
            if (vertex != state.m_komove
                && !board.is_suicide(vertex, board.get_to_move())) {
                nodelist.emplace_back(node);
                legal_sum += node.first;
            }
        } else {
            nodelist.emplace_back(node);
            legal_sum += node.first;
        }
    }

    // If the sum is 0 or a denormal, then don't try to normalize.
    if (legal_sum > std::numeric_limits<float>::min()) {
        // re-normalize after removing illegal moves.
        for (auto& node : nodelist) {
            node.first /= legal_sum;
        }
    }

    link_nodelist(nodecount, nodelist, net_eval);
    return true;
}

void UCTNode::link_nodelist(std::atomic<int> & nodecount,
                            std::vector<Network::scored_node> & nodelist,
                            float init_eval)
{
    auto totalchildren = nodelist.size();
    if (!totalchildren) {
        return;
    }

    // sort (this will reverse scores, but linking is backwards too)
    std::sort(rbegin(nodelist), rend(nodelist));

    LOCK(get_mutex(), lock);

    for (const auto& node : nodelist) {
        auto child = new UCTNode(node.second, node.first, init_eval);
        children.push_back(child);
    }

    nodecount += children.size();
    m_has_children = true;
}

void UCTNode::kill_superkos(KoState & state) {

    auto childIter = begin(children);
    while (childIter != end(children)) {
        int move = (*childIter)->get_move();

        if (move != FastBoard::PASS) {
            KoState mystate = state;
            mystate.play_move(move);

            if (mystate.superko()) {
                childIter = children.erase(childIter);
                continue;
            }
        }
        childIter++;
    }
}

float UCTNode::eval_state(GameState& state) {
    auto raw_netlist = Network::get_scored_moves(
        &state, Network::Ensemble::RANDOM_ROTATION);

    // DCNN returns winrate as side to move
    auto net_eval = raw_netlist.second;

    // But we score from black's point of view
    if (state.get_to_move() == FastBoard::WHITE) {
        net_eval = 1.0f - net_eval;
    }

    return net_eval;
}

void UCTNode::dirichlet_noise(float epsilon, float alpha) {
    auto child_cnt = children.size();

    auto dirichlet_vector = std::vector<float>{};
    std::gamma_distribution<float> gamma(alpha, 1.0f);
    for (size_t i = 0; i < child_cnt; i++) {
        dirichlet_vector.emplace_back(gamma(Random::get_Rng()));
    }

    auto sample_sum = std::accumulate(begin(dirichlet_vector),
                                      end(dirichlet_vector), 0.0f);

    // If the noise vector sums to 0 or a denormal, then don't try to
    // normalize.
    if (sample_sum < std::numeric_limits<float>::min()) {
        return;
    }

    for (auto& v: dirichlet_vector) {
        v /= sample_sum;
    }

    child_cnt = 0;
    for (auto& child : children) {
        auto score = child->get_score();
        auto eta_a = dirichlet_vector[child_cnt++];
        score = score * (1 - epsilon) + epsilon * eta_a;
        child->set_score(score);
    }
}

void UCTNode::randomize_first_proportionally() {
    auto accum = uint32{0};
    auto accum_vector = std::vector<uint32>{};
    for (const auto& child : children) {
        accum += child->get_visits();
        accum_vector.emplace_back(accum);
    }

    auto pick = Random::get_Rng().randuint32(accum);
    auto index = size_t{0};
    for (size_t i = 0; i < accum_vector.size(); i++) {
        if (pick < accum_vector[i]) {
            index = i;
            break;
        }
    }

    // Take the early out
    if (index == 0) {
        return;
    }

    assert(children.size() >= index);

    // Now swap the child at index with the first child
    std::iter_swap(begin(children), begin(children) + index);
}

int UCTNode::get_move() const {
    return m_move;
}

void UCTNode::virtual_loss() {
    m_virtual_loss += VIRTUAL_LOSS_COUNT;
}

void UCTNode::virtual_loss_undo() {
    m_virtual_loss -= VIRTUAL_LOSS_COUNT;
}

void UCTNode::update(float eval) {
    m_visits++;
    accumulate_eval(eval);
}

bool UCTNode::has_children() const {
    return m_has_children;
}

void UCTNode::set_visits(int visits) {
    m_visits = visits;
}

float UCTNode::get_score() const {
    return m_score;
}

void UCTNode::set_score(float score) {
    m_score = score;
}

int UCTNode::get_visits() const {
    return m_visits;
}

float UCTNode::get_eval(int tomove) const {
    // Due to the use of atomic updates and virtual losses, it is
    // possible for the visit count to change underneath us. Make sure
    // to return a consistent result to the caller by caching the values.
    auto virtual_loss = int{m_virtual_loss};
    auto visits = get_visits() + virtual_loss;
    if (visits > 0) {
        auto blackeval = get_blackevals();
        if (tomove == FastBoard::WHITE) {
            blackeval += static_cast<double>(virtual_loss);
        }
        auto score = static_cast<float>(blackeval / (double)visits);
        if (tomove == FastBoard::WHITE) {
            score = 1.0f - score;
        }
        return score;
    } else {
        // If a node has not been visited yet,
        // the eval is that of the parent.
        auto eval = m_init_eval;
        if (tomove == FastBoard::WHITE) {
            eval = 1.0f - eval;
        }
        return eval;
    }
}

double UCTNode::get_blackevals() const {
    return m_blackevals;
}

void UCTNode::set_blackevals(double blackevals) {
    m_blackevals = blackevals;
}

void UCTNode::accumulate_eval(float eval) {
    atomic_add(m_blackevals, (double)eval);
}

UCTNode* UCTNode::uct_select_child(int color) {
    UCTNode * best = nullptr;
    float best_value = -1000.0f;

    LOCK(get_mutex(), lock);

    // Count parentvisits.
    // We do this manually to avoid issues with transpositions.
    int parentvisits = 0;
    for (const auto& child : children) {
        if (child->valid()) {
            parentvisits += child->get_visits();
        }
    }
    float numerator = std::sqrt((double)parentvisits);

    for (const auto& child : children) {
        if (!child->valid()) {
            continue;
        }

        // get_eval() will automatically set first-play-urgency
        float winrate = child->get_eval(color);
        float psa = child->get_score();
        float denom = 1.0f + child->get_visits();
        float puct = cfg_puct * psa * (numerator / denom);
        float value = winrate + puct;
        assert(value > -1000.0f);

        if (value > best_value) {
            best_value = value;
            best = child;
        }
    }

    assert(best != nullptr);
    return best;
}

class NodeComp : public std::binary_function<UCTNode::sortnode_t,
                                             UCTNode::sortnode_t, bool> {
public:
    NodeComp() = default;
    // winrate, visits, score, child
    //        0,     1,     2,     3

    bool operator()(const UCTNode::sortnode_t a, const UCTNode::sortnode_t b) {
        // One node has visits, the other does not
        if (!std::get<1>(a) && std::get<1>(b)) {
            return false;
        }

        if (!std::get<1>(b) && std::get<1>(a)) {
            return true;
        }

        // Neither has visits, sort on prior score
        if (!std::get<1>(a) && !std::get<1>(b)) {
            return std::get<2>(a) > std::get<2>(b);
        }

        // Both have visits, but the same amount, prefer winrate
        if (std::get<1>(a) == std::get<1>(b)) {
            return std::get<0>(a) > std::get<0>(b);
        }

        // Both have different visits, prefer greater visits
        return std::get<1>(a) > std::get<1>(b);
    }
};

void UCTNode::sort_root_children(int color) {
    LOCK(get_mutex(), lock);
    auto tmp = std::vector<sortnode_t>{};

    for (const auto& child : children) {
        auto visits = child->get_visits();
        auto score = child->get_score();
        if (visits) {
            auto winrate = child->get_eval(color);
            tmp.emplace_back(winrate, visits, score, child);
        } else {
            tmp.emplace_back(0.0f, 0, score, child);
        }
    }

    std::stable_sort(rbegin(tmp), rend(tmp), NodeComp());
    std::reverse(begin(tmp), end(tmp));

    // I didn't get the same result with
    // std::stable_sort(begin(tmp), end(tmp), NodeComp());
    // maybe because of stable?

    children.clear();
    for (const auto& sortnode : tmp) {
        children.push_back(std::get<3>(sortnode));
    }
}

/**
 * Helper function to get a sortnode_t
 * eval is set to 0 if no visits instead of first-play-urgency
 */
UCTNode::sortnode_t get_sortnode(int color, UCTNode* child) {
    auto visits = child->get_visits();
    return UCTNode::sortnode_t(
        visits == 0 ? 0.0f : child->get_eval(color),
        visits,
        child->get_score(),
        child);
}

UCTNode* UCTNode::get_best_root_child(int color) {
    LOCK(get_mutex(), lock);
    assert(!children.empty());

    NodeComp compare;
    auto best_child = get_sortnode(color, children[0]);
    for (const auto& child : children) {
        auto test = get_sortnode(color, child);
        if (compare(test, best_child)) {
            best_child = test;
        }
    }
    return std::get<3>(best_child);
}

UCTNode* UCTNode::get_first_child() const {
    if (children.empty()) {
        return nullptr;
    }
    return children.front();
}

const std::vector<UCTNode*> UCTNode::get_children() const {
    return children;
}

UCTNode* UCTNode::get_nopass_child(FastState& state) const {
    for (const auto& child : children) {
        /* If we prevent the engine from passing, we must bail out when
           we only have unreasonable moves to pick, like filling eyes.
           Note that this isn't knowledge isn't required by the engine,
           we require it because we're overruling its moves. */
        if (child->m_move != FastBoard::PASS
            && !state.board.is_eye(state.get_to_move(), child->m_move)) {
            return child;
        }
    }
    return nullptr;
}

void UCTNode::invalidate() {
    m_valid = false;
}

bool UCTNode::valid() const {
    return m_valid;
}

// unsafe in SMP, we don't know if people hold pointers to the
// child which they might dereference
void UCTNode::delete_child(UCTNode * del_child) {
    LOCK(get_mutex(), lock);
    assert(del_child != nullptr);

    auto found = std::find(begin(children), end(children), del_child);
    if (found == end(children)) {
        assert(false && "Child to delete not found");
    }

    children.erase(found);
    delete *found;
}
