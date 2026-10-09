#pragma once

namespace GroupSortMethod {
    enum GroupSortMethod {
        Raw,
        ByType,
        ByAddress,
        ByName,
        ByTestResult,
        ById,
        ByTraffic,
        BySecurity,
        ByLatency, // Dedicated URL sweep ordering, independent of the manual test-result preference.
    };
}

struct GroupSortAction {
    GroupSortMethod::GroupSortMethod method = GroupSortMethod::Raw;
    bool descending = false;
};
