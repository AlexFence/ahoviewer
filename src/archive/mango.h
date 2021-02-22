#pragma once

#include "archive.h"

namespace AhoViewer
{
    class Mango : public Archive
    {
    public:
        Mango(const std::string &path, const std::string &exDir);
        virtual ~Mango() override = default;

        virtual bool extract(const std::string &file) const override;
        virtual bool has_valid_files(const FileType t) const override;
        virtual std::vector<std::string> get_entries(const FileType t) const override;

        static const int MagicSize = 0;
        static const char Magic[MagicSize];
    };
}
