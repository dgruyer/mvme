#ifndef __MVME_A2__IMPL_H__
#define __MVME_A2__IMPL_H__

#include "a2.h"

namespace a2
{

enum OperatorType
{
    Operator_Calibration,
    Operator_Calibration_sse,
    Operator_KeepPrevious,
    Operator_Difference,
    Operator_Difference_idx,
    Operator_ArrayMap,
    Operator_BinaryEquation,
    Operator_H1DSink,
    Operator_H2DSink,
    Operator_RangeFilter,
    Operator_Sum,
    Operator_Multiplicity,
    Operator_Max,

    OperatorTypeMax
};

void calibration_step(Operator *op);
void calibration_step_sse(Operator *op);
void keep_previous_step(Operator *op);
void difference_step(Operator *op);
void array_map_step(Operator *op);
void binary_equation_step(Operator *op);
void h1d_sink_step(Operator *op);

} // namespace a2

#endif /* __MVME_A2__IMPL_H__ */
