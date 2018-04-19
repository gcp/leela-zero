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

#include "SMP.h"

#include <cassert>
#include <thread>

SMP::Mutex::Mutex() {
    m_lock = false;
}

SMP::Lock::Lock(Mutex & m) {
    m_mutex = &m;
    m_selflock = false;
    lock();
}

void SMP::Lock::lock() {
    assert(!m_selflock);
    while (m_mutex->m_lock.exchange(true, std::memory_order_acquire) == true);
    m_selflock = true;
}

void SMP::Lock::unlock() {
    assert(m_selflock);
    auto prevval = m_mutex->m_lock.exchange(false, std::memory_order_release);

    // if this fails it means we are unlocking an unlocked lock
    assert(prevval);
    m_selflock = false;
}

SMP::Lock::~Lock() {
    // if we don't claim to hold the lock,
    // don't bother trying to unlock on destructor.
    if (m_selflock) {
        unlock();
    }
}

int SMP::get_num_cpus() {
    return std::thread::hardware_concurrency();
}
