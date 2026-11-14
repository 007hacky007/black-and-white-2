import Foundation

/// The steps `python -m bw2.builder_cli build` announces, in order, with their share of the bar (they add up
/// to 100): bw2/builder_cli.py STEPS and WEIGHTS, from the end-to-end build's wall times (plan 13 Task 6 Step 6:
/// tools 32.6 s, discs 12.6 s, install 17.9 s, patches 160.1 s, lift 125.3 s, compile 185.5 s, package 21.8 s,
/// on a busy Mac). tests/test_builder_swift.py compares this list with the Python one.
public let buildSteps: [(id: String, weight: Double)] = [
    ("tools", 6), ("discs", 2), ("install", 3), ("patches", 29), ("lift", 23), ("compile", 33), ("package", 4),
]

/// Fraction (0...1) of the whole build done when the given step begins.
public func progressFraction(atStart stepID: String) -> Double {
    var sum = 0.0
    for step in buildSteps {
        if step.id == stepID { break }
        sum += step.weight
    }
    return sum / 100
}

/// Fraction (0...1) of the whole build done when `part` (0...1) of the given step is done.
public func progressFraction(in stepID: String, part: Double) -> Double {
    let weight = buildSteps.first { $0.id == stepID }?.weight ?? 0
    return min(1, progressFraction(atStart: stepID) + weight * max(0, min(1, part)) / 100)
}
