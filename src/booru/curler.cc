#include <fstream>

#include "curler.h"
using namespace AhoViewer::Booru;

// Used for looking closer at what libcurl is doing
// set it to 1 with CXXFLAGS and prepare to be spammed
#ifndef VERBOSE_LIBCURL
#define VERBOSE_LIBCURL 0
#endif

const char *Curler::UserAgent = "Mozilla/5.0";

size_t Curler::write_cb(const unsigned char *ptr, size_t size, size_t nmemb, void *userp)
{
    Curler *self = static_cast<Curler*>(userp);

    if (self->is_cancelled())
        return 0;

    size_t len = size * nmemb;

    self->m_Buffer.insert(self->m_Buffer.end(), ptr, ptr + len);
    self->m_SignalWrite(ptr, len);

    if (self->m_DownloadTotal == 0)
    {
        double s;
        curl_easy_getinfo(self->m_EasyHandle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &s);
        self->m_DownloadTotal = s;
    }

    self->m_DownloadCurrent = self->m_Buffer.size();

    if (!self->is_cancelled())
        self->m_SignalProgress();

    return len;
}

int Curler::progress_cb(void *userp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    Curler *self = static_cast<Curler*>(userp);

    // This callback is called way too frequently and will
    // lock up the UI thread if the progress is dispatched from it
    /*
    self->m_DownloadTotal = dlt;
    self->m_DownloadCurrent = dln;

    if (!self->is_cancelled())
        self->m_SignalProgress();
    */

    return self->is_cancelled();
}

Curler::Curler(const std::string &url, CURLSH *share)
  : m_EasyHandle(curl_easy_init()),
    m_Active(false),
    m_DownloadTotal(0),
    m_DownloadCurrent(0),
    m_Cancel(Gio::Cancellable::create())
{
    curl_easy_setopt(m_EasyHandle, CURLOPT_USERAGENT, UserAgent);
    curl_easy_setopt(m_EasyHandle, CURLOPT_WRITEFUNCTION, &Curler::write_cb);
    curl_easy_setopt(m_EasyHandle, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(m_EasyHandle, CURLOPT_XFERINFOFUNCTION, &Curler::progress_cb);
    curl_easy_setopt(m_EasyHandle, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(m_EasyHandle, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(m_EasyHandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(m_EasyHandle, CURLOPT_MAXREDIRS, 5);
    curl_easy_setopt(m_EasyHandle, CURLOPT_FAILONERROR, 1);
    curl_easy_setopt(m_EasyHandle, CURLOPT_VERBOSE, VERBOSE_LIBCURL);
    curl_easy_setopt(m_EasyHandle, CURLOPT_CONNECTTIMEOUT, 60);
    curl_easy_setopt(m_EasyHandle, CURLOPT_NOSIGNAL, 1);

#ifdef _WIN32
    HMODULE hModule = GetModuleHandle(NULL);
    char m[MAX_PATH];
    GetModuleFileName(hModule, m, MAX_PATH);
    std::string d(m),
                cert_path = Glib::build_filename(d.substr(0, d.rfind('\\')), "curl-ca-bundle.crt");
    curl_easy_setopt(m_EasyHandle, CURLOPT_CAINFO, cert_path.c_str());
#endif // _WIN32

    if (!url.empty())
        set_url(url);

    if (share)
        set_share_handle(share);
}

Curler::~Curler()
{
    curl_easy_cleanup(m_EasyHandle);
}

void Curler::set_url(const std::string &url)
{
    m_Url = url;
    curl_easy_setopt(m_EasyHandle, CURLOPT_URL, m_Url.c_str());
}

void Curler::set_no_body(const bool n) const
{
    curl_easy_setopt(m_EasyHandle, CURLOPT_NOBODY, n);
}

void Curler::set_follow_location(const bool n) const
{
    curl_easy_setopt(m_EasyHandle, CURLOPT_FOLLOWLOCATION, n);
}

void Curler::set_referer(const std::string &url) const
{
    curl_easy_setopt(m_EasyHandle, CURLOPT_REFERER, url.c_str());
}

void Curler::set_http_auth(const std::string &u, const std::string &p) const
{
    if (!u.empty())
    {
        curl_easy_setopt(m_EasyHandle, CURLOPT_USERNAME, u.c_str());
        curl_easy_setopt(m_EasyHandle, CURLOPT_PASSWORD, p.c_str());
    }
}

void Curler::set_cookie_jar(const std::string &path) const
{
    curl_easy_setopt(m_EasyHandle, CURLOPT_COOKIEJAR, path.c_str());
}

void Curler::set_cookie_file(const std::string &path) const
{
    curl_easy_setopt(m_EasyHandle, CURLOPT_COOKIEFILE, path.c_str());
}

void Curler::set_post_fields(const std::string &fields) const
{
    curl_easy_setopt(m_EasyHandle, CURLOPT_POSTFIELDSIZE, fields.length());
    curl_easy_setopt(m_EasyHandle, CURLOPT_POSTFIELDS, fields.c_str());
}

void Curler::set_share_handle(CURLSH *s) const
{
    curl_easy_setopt(m_EasyHandle, CURLOPT_SHARE, s);
}

std::string Curler::escape(const std::string &str) const
{
    std::string r;
    char *s = curl_easy_escape(m_EasyHandle, str.c_str(), str.length());

    if (s)
    {
        r = s;
        curl_free(s);
    }

    return r;
}

bool Curler::perform(const size_t rCount)
{
    size_t i = 0;
    m_Cancel->reset();
    clear();

    // XXX: Ocassionally Danbooru (cloudflare) returns a 500 internal server error
    // this is most likely a temporary issue or could even be my ISP's fault,
    // but this is only used when fetching the posts xml so it should be fine
    do
    {
        m_Response = curl_easy_perform(m_EasyHandle);
    }
    while (m_Response != CURLE_OK && ++i < rCount && !is_cancelled());

    return m_Response == CURLE_OK;
}

void Curler::get_progress(double &current, double &total)
{
    current = m_DownloadCurrent;
    total   = m_DownloadTotal;
}

long Curler::get_response_code() const
{
    long c;
    curl_easy_getinfo(m_EasyHandle, CURLINFO_RESPONSE_CODE, &c);

    return c;
}

void Curler::save_file(const std::string &path) const
{
    std::ofstream ofs(path, std::ofstream::binary);

    if (ofs)
        std::copy(m_Buffer.begin(), m_Buffer.end(), std::ostream_iterator<unsigned char>(ofs));
}
