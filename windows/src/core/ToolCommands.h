#pragma once
#include "Editing.h"
namespace comp {
QJsonArray extendedTools();
void validateToolAdjustment(QString kind, const QJsonObject& parameters);
bool executeExtended(Document&, const EditCommand&, const CancelCheck&);
FloatImage applyWarp(const FloatImage&, const QJsonObject&, double, const CancelCheck&);
} // namespace comp
