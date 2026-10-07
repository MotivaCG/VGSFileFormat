#pragma once
#include "project.h"
#include <functional>
#include <vector>

// Adapted from SMNForge's pruneIsolation: d_N excludes the query point itself.
// The global upper median is measured only on surviving finite world centres.
void applyIsolation(const std::vector<QVector3D> &positions,std::vector<uint8_t> &keep,
                    const QVector<IsolationFilter> &,const std::function<bool()> &cancelled = {});
