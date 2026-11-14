import Foundation

// The window comes with plan 13 Task 8; until then the executable stops loudly.
FileHandle.standardError.write(Data("[bw2] reached the Builder window: owned by a later plan (plan 13 Task 8)\n".utf8))
exit(4)
