#pragma once

#include <cstdint>
#include <vector>

// 根据已经扣除当前软件补偿的固有到达延迟，计算最小非负补偿。
// 返回结果中，固有延迟最大的设备补偿恒为 0，其余设备只延后两者的差值。
std::vector<double> calculateMinimalAcousticDelays(
    const std::vector<double>& underlyingLatenciesMilliseconds,
    double maximumDelayMilliseconds = 500.0);

// 把捕获端的绝对采样帧和发送端的 QPC 时刻投影到同一个“延迟坐标”。
// 两个时钟的未知原点会形成公共常数，但设备之间作差时会抵消。
double calculateCrossClockArrivalCoordinateMilliseconds(
    std::uint64_t arrivalFrame,
    double captureFramesPerSecond,
    std::int64_t probeStartQpcHundredNanoseconds);
