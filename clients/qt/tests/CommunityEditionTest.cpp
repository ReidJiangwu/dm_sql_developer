#include "../src/core/ProductFeatures.h"
#include "../src/core/BuiltInSnippets.h"
#include <QCoreApplication>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (ProductFeatures::erp || ProductFeatures::internalHub) return 1;
    for (const auto& snippet : builtInSnippets())
        if (snippet.id == QStringLiteral("builtin-fnd-user")) return 4;
    std::puts("COMMUNITY_FEATURE_BOUNDARY=PASS");
    return 0;
}
