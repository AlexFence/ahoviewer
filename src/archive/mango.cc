#pragma once

#include "../config.h"

#ifdef HAVE_LIBMANGO
#include "mango.h"
using namespace AhoViewer;

#include <iostream>
#include <giomm.h>

extern "C" {
    #include <libmango.h>
}

const char Mango::Magic[Mango::MagicSize] = {};

Mango::Mango(const std::string &path, const std::string &exDir)
  : Archive::Archive(path, exDir)
{

}

bool Mango::extract(const std::string &file) const
{
    printf("requested img: %s\n", file.c_str());
    bool found = false;
    int err = -2;
    MangoFile mango = mangofile_open(const_cast<char*>(m_Path.c_str()), &err);



    if (err == 0)
    {
        std::string fPath = Glib::build_filename(m_ExtractedPath, file);

        if (!Glib::file_test(Glib::path_get_dirname(fPath), Glib::FILE_TEST_EXISTS))
            g_mkdir_with_parents(Glib::path_get_dirname(fPath).c_str(), 0755);


        MangoImage mango_img = NULL;

        for(size_t i = 0; i < mangofile_get_image_count(mango); i++) 
        {
            MangoImage img = mangofile_get_image(mango, i);
            MangoImageMeta meta = mangoimg_get_meta(img);
            
            std::string img_name(mangoimgmeta_filename(meta));
            
            if (file.compare(img_name) == 0) 
            {
                printf("found img: %s\n", mangoimgmeta_filename(meta));
                mango_img = img;
                break;
            } 
        }
        

        if (mango_img != NULL)
        {
            mangoimg_uncompress(mango_img);
            ImageData img_data = mangoimg_get_image_data(mango_img);

            
            auto f = Gio::File::create_for_path(fPath);
            auto ofs = f->replace();
            ofs->write(img_data.pointer, img_data.length);

            found = true;
        }


        mangofile_free(mango);
    }

    return found;
}

bool Mango::has_valid_files(const FileType t) const
{
    return !get_entries(t).empty();
}

std::vector<std::string> Mango::get_entries(const FileType t) const
{
    std::vector<std::string> entries;
    int err = -2;
    MangoFile mango = mangofile_open(const_cast<char*>(m_Path.c_str()), &err);

    if (err == 0)
    {
        for (size_t i = 0, n = mangofile_get_image_count(mango); i < n; ++i)
        {
            MangoImage mango_image = mangofile_get_image(mango, i);
            MangoImageMeta meta = mangoimg_get_meta(mango_image);
            std::string file_name(mangoimgmeta_filename(meta));

            if (Image::is_valid_extension(file_name)) 
            {
                entries.emplace_back(file_name);
            }
        }

        mangofile_free(mango);
    }

    return entries;
}
#endif
