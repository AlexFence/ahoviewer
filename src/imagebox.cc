#include "imagebox.h"
using namespace AhoViewer;

#include "settings.h"
#include "statusbar.h"

#ifdef HAVE_GSTREAMER
#include <gst/video/videooverlay.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif // GDK_WINDOWING_X11
#ifdef GDK_WINDOWING_WIN32
#include <gdk/gdkwin32.h>
#endif // GDK_WINDOWING_WIN32

GstBusSyncReply ImageBox::create_window(GstBus*, GstMessage *message, void *userp)
{
    if (!gst_is_video_overlay_prepare_window_handle_message(message))
        return GST_BUS_PASS;

    ImageBox *self = static_cast<ImageBox*>(userp);
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(self->m_VideoSink), self->m_WindowHandle);

    gst_message_unref(message);

    return GST_BUS_DROP;
}

gboolean ImageBox::bus_cb(GstBus*, GstMessage *message, void *userp)
{
    ImageBox *self = static_cast<ImageBox*>(userp);

    switch (GST_MESSAGE_TYPE(message))
    {
        case GST_MESSAGE_EOS:
            gst_element_seek_simple(self->m_Playbin, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0);
        default:
            break;
    }

    return TRUE;
}
#endif // HAVE_GSTREAMER

const double ImageBox::SmoothScrollStep = 1000.0 / 60.0;

ImageBox::ImageBox(BaseObjectType *cobj, const Glib::RefPtr<Gtk::Builder> &bldr)
  : Gtk::EventBox(cobj),
    m_LeftPtrCursor(Gdk::LEFT_PTR),
    m_FleurCursor(Gdk::FLEUR),
    m_BlankCursor(Gdk::BLANK_CURSOR),
    m_WindowWidth(0),
    m_WindowHeight(0),
    m_LayoutWidth(0),
    m_LayoutHeight(0),
    m_RedrawQueued(false),
    m_HideScrollbars(false),
    m_ZoomScroll(false),
    m_ZoomMode(Settings.get_zoom_mode()),
    m_ZoomPercent(100)
{
    bldr->get_widget("ImageBox::Layout",       m_Layout);
    bldr->get_widget("ImageBox::HScroll",      m_HScroll);
    bldr->get_widget("ImageBox::VScroll",      m_VScroll);
    bldr->get_widget("ImageBox::Image",        m_GtkImage);
    bldr->get_widget("ImageBox::DrawingArea",  m_DrawingArea);

    m_HAdjust = Glib::RefPtr<Gtk::Adjustment>::cast_static(bldr->get_object("ImageBox::HAdjust"));
    m_VAdjust = Glib::RefPtr<Gtk::Adjustment>::cast_static(bldr->get_object("ImageBox::VAdjust"));
    m_UIManager = Glib::RefPtr<Gtk::UIManager>::cast_static(bldr->get_object("UIManager"));

#ifdef HAVE_GSTREAMER
    m_Playbin   = gst_element_factory_make("playbin", "playbin"),
    m_VideoSink = gst_element_factory_make("glimagesink", "videosink");

    g_object_set(m_Playbin,
            "audio-sink", gst_element_factory_make("fakesink", "audiosink"),
            "video-sink", m_VideoSink,
            NULL);

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(m_Playbin));
    gst_bus_add_watch(bus, &ImageBox::bus_cb, this);
    gst_bus_set_sync_handler(bus, &ImageBox::create_window, this, NULL);
    g_object_unref(bus);

    m_DrawingArea->signal_realize().connect([ this ]()
    {
#ifdef GDK_WINDOWING_X11
        m_WindowHandle = GDK_WINDOW_XID(m_DrawingArea->get_window()->gobj());
#endif // GDK_WINDOWING_X11
#ifdef GDK_WINDOWING_WIN32
        m_WindowHandle = (guintptr)GDK_WINDOW_HWND(m_DrawingArea->get_window()->gobj());
#endif // GDK_WINDOWING_WIN32
        gst_element_set_state(m_Playbin, GST_STATE_READY);
     });
#endif // HAVE_GSTREAMER
}

void ImageBox::queue_draw_image(const bool scroll)
{
    if (!get_realized() || !m_Image || (m_RedrawQueued && !(scroll || m_FirstDraw)))
        return;

    m_DrawConn.disconnect();
    m_RedrawQueued = true;
    m_DrawConn = Glib::signal_idle().connect(
            sigc::bind_return(sigc::bind(sigc::mem_fun(*this, &ImageBox::draw_image), scroll), false),
            Glib::PRIORITY_HIGH_IDLE);
}

void ImageBox::set_image(const std::shared_ptr<Image> &image)
{
    m_ImageConn.disconnect();
    reset_slideshow();

#ifdef HAVE_GSTREAMER
    gst_element_set_state(m_Playbin, GST_STATE_NULL);
    m_Playing = false;
    m_DrawingArea->hide();
#endif // HAVE_GSTREAMER

    m_Image = image;
    m_FirstDraw = true;
    queue_draw_image(true);
    m_ImageConn = m_Image->signal_pixbuf_changed().connect(
            sigc::bind(sigc::mem_fun(*this, &ImageBox::queue_draw_image), false));
}

void ImageBox::clear_image()
{
    m_SlideshowConn.disconnect();
    m_ImageConn.disconnect();
    m_DrawConn.disconnect();
    m_AnimConn.disconnect();
    m_GtkImage->clear();
    m_DrawingArea->hide();
    m_Layout->set_size(0, 0);

#ifdef HAVE_GSTREAMER
    gst_element_set_state(m_Playbin, GST_STATE_NULL);
    m_Playing = false;
#endif // HAVE_GSTREAMER

    m_HScroll->hide();
    m_VScroll->hide();

    m_StatusBar->clear_resolution();
    m_Image = nullptr;
}

void ImageBox::update_background_color()
{
    m_BGColor = Settings.get_background_color();
    m_Layout->modify_bg(Gtk::STATE_NORMAL, m_BGColor);
}

void ImageBox::reset_slideshow()
{
    if (m_SlideshowConn)
    {
        m_SlideshowConn.disconnect();
        toggle_slideshow();
    }
}

void ImageBox::toggle_slideshow()
{
    if (!m_SlideshowConn)
    {
        m_SlideshowConn = Glib::signal_timeout().connect_seconds(
                sigc::mem_fun(*this, &ImageBox::advance_slideshow), Settings.get_int("SlideshowDelay"));
    }
    else
    {
        m_SlideshowConn.disconnect();
    }
}

ImageBox::ZoomMode ImageBox::get_zoom_mode() const
{
    return m_ZoomMode;
}

void ImageBox::set_zoom_mode(const ZoomMode mode)
{
    if (mode != m_ZoomMode)
    {
        Settings.set_zoom_mode(mode);
        m_ZoomMode = mode;
        queue_draw_image(true);
    }
}

void ImageBox::set_statusbar(StatusBar *sb)
{
    m_StatusBar = sb;
}

void ImageBox::on_zoom_in()
{
    zoom(m_ZoomPercent + 10);
}

void ImageBox::on_zoom_out()
{
    zoom(m_ZoomPercent - 10);
}

void ImageBox::on_reset_zoom()
{
    zoom(100);
}

void ImageBox::on_toggle_scrollbars()
{
    Settings.set("ScrollbarsVisible", !Settings.get_bool("ScrollbarsVisible"));
    queue_draw_image();
}

void ImageBox::on_scroll_up()
{
    scroll(0, -300);
}

void ImageBox::on_scroll_down()
{
    scroll(0, 300);
}

void ImageBox::on_scroll_left()
{
    scroll(-300, 0);
}

void ImageBox::on_scroll_right()
{
    scroll(300, 0);
}

void ImageBox::on_realize()
{
    m_PopupMenu = static_cast<Gtk::Menu*>(m_UIManager->get_widget("/PopupMenu"));

    Glib::RefPtr<Gtk::ActionGroup> actionGroup =
        static_cast<std::vector<Glib::RefPtr<Gtk::ActionGroup>>>(m_UIManager->get_action_groups())[0];

    m_NextAction = actionGroup->get_action("NextImage");
    m_PreviousAction = actionGroup->get_action("PreviousImage");

    update_background_color();

    Gtk::EventBox::on_realize();
}

bool ImageBox::on_button_press_event(GdkEventButton *e)
{
    grab_focus();
    cursor_timeout();

    // Ignore double/triple clicks
    if (e->type == GDK_BUTTON_PRESS)
    {
        switch (e->button)
        {
            case 1:
            case 2:
                m_PressX = m_PreviousX = e->x_root;
                m_PressY = m_PreviousY = e->y_root;
                return true;
            case 3:
                m_PopupMenu->popup(e->button, e->time);
                m_CursorConn.disconnect();
                return true;
            case 8: // Back
                m_PreviousAction->activate();
                return true;
            case 9: // Forward
                m_NextAction->activate();
                return true;
        }
    }

    return Gtk::EventBox::on_button_press_event(e);
}

bool ImageBox::on_button_release_event(GdkEventButton *e)
{
    if (e->button == 1 || e->button == 2)
    {
        m_Layout->get_window()->set_cursor(m_LeftPtrCursor);

        if (e->button == 1 && m_PressX == m_PreviousX && m_PressY == m_PreviousY)
            m_NextAction->activate();

        return true;
    }

    return Gtk::EventBox::on_button_release_event(e);
}

bool ImageBox::on_motion_notify_event(GdkEventMotion *e)
{
    if (m_Image && ((e->state & GDK_BUTTON1_MASK) == GDK_BUTTON1_MASK ||
                    (e->state & GDK_BUTTON2_MASK) == GDK_BUTTON2_MASK))
    {
        m_Layout->get_window()->set_cursor(m_FleurCursor);
        scroll(m_PreviousX - e->x_root, m_PreviousY - e->y_root, true);

        m_PreviousX = e->x_root;
        m_PreviousY = e->y_root;

        return true;
    }
    else
    {
        cursor_timeout();
    }

    return Gtk::EventBox::on_motion_notify_event(e);
}

bool ImageBox::on_scroll_event(GdkEventScroll *e)
{
    grab_focus();
    cursor_timeout();

    switch (e->direction)
    {
        case GDK_SCROLL_UP:
            if ((e->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
                on_zoom_in();
            else
                scroll(0, -300);
            return true;
        case GDK_SCROLL_DOWN:
            if ((e->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
                on_zoom_out();
            else
                scroll(0, 300);
            return true;
        case GDK_SCROLL_LEFT:
            scroll(-300, 0);
            return true;
        case GDK_SCROLL_RIGHT:
            scroll(300, 0);
            return true;
    }

    return Gtk::EventBox::on_scroll_event(e);
}

void ImageBox::draw_image(bool scroll)
{
    get_window()->freeze_updates();

    m_HScroll->show();
    m_VScroll->show();

    while (Gtk::Main::events_pending())
        Gtk::Main::iteration();

    if (!m_Image || (!m_Image->is_webm() && !m_Image->get_pixbuf()) ||
        (m_Image->is_webm() && m_Image->is_loading()))
    {
        get_window()->thaw_updates();
        m_RedrawQueued = false;
        return;
    }

    m_WindowWidth  = get_allocation().get_width();
    m_WindowHeight = get_allocation().get_height();
    m_LayoutWidth  = m_Layout->get_allocation().get_width();
    m_LayoutHeight = m_Layout->get_allocation().get_height();

    int w       = m_LayoutWidth,
        h       = m_LayoutHeight,
        sWidth  = 0,
        sHeight = 0,
        origWidth, origHeight;

    Glib::RefPtr<Gdk::Pixbuf> temp;

    m_HideScrollbars = !Settings.get_bool("ScrollbarsVisible") || Settings.get_bool("HideAll");

    // if the image is still loading we want to draw all requests
    m_FirstDraw = m_Image->is_loading();

#ifdef HAVE_GSTREAMER
    bool start_playing = false;
    if (m_Image->is_webm())
    {
        if (!m_Playing)
        {
            m_GtkImage->clear();
            m_DrawingArea->show();

            g_object_set(m_Playbin, "uri", Glib::filename_to_uri(m_Image->get_path()).c_str(), NULL);
            gst_element_set_state(m_Playbin, GST_STATE_PAUSED);
            gst_element_get_state(m_Playbin, NULL, NULL, GST_CLOCK_TIME_NONE);
            start_playing = m_Playing = true;
        }

        GstPad *pad = nullptr;
        g_signal_emit_by_name(m_Playbin, "get-video-pad", 0, &pad, NULL);

        GstCaps *caps = gst_pad_get_current_caps(pad);
        GstStructure *s = gst_caps_get_structure(caps, 0);

        gst_structure_get_int(s, "width", &origWidth);
        gst_structure_get_int(s, "height", &origHeight);

        gst_caps_unref(caps);
        gst_object_unref(pad);

        if (origHeight > 0 && origHeight > 0)
            get_scaled_size(origWidth, origHeight, sWidth, sHeight);
    }
    else
    {
#endif // HAVE_GSTREAMER
        Glib::RefPtr<Gdk::PixbufAnimation> pixbuf_anim = m_Image->get_pixbuf();

        if (m_PixbufAnim != pixbuf_anim)
        {
            m_PixbufAnim = pixbuf_anim;

            // https://bugzilla.gnome.org/show_bug.cgi?id=688686
            if (m_PixbufAnimIter)
            {
                g_object_unref(m_PixbufAnimIter->gobj());
                m_PixbufAnimIter.reset();
            }

            m_PixbufAnimIter = m_PixbufAnim->get_iter(NULL);

            m_AnimConn.disconnect();

            if (m_PixbufAnimIter->get_delay_time() >= 0)
                m_AnimConn = Glib::signal_timeout().connect(
                        sigc::mem_fun(*this, &ImageBox::update_animation),
                        m_PixbufAnimIter->get_delay_time());
        }

        Glib::RefPtr<Gdk::Pixbuf> pixbuf = m_PixbufAnimIter->get_pixbuf()->copy();
        temp = pixbuf;

        origWidth  = pixbuf->get_width();
        origHeight = pixbuf->get_height();

        if (get_scaled_size(pixbuf->get_width(), pixbuf->get_height(), sWidth, sHeight))
            temp = pixbuf->scale_simple(sWidth, sHeight, Gdk::INTERP_BILINEAR);
#ifdef HAVE_GSTREAMER
    }
#endif // HAVE_GSTREAMER

    if (m_HideScrollbars || sWidth <= m_LayoutWidth || (sWidth <= m_WindowWidth && sHeight <= m_WindowHeight))
    {
        m_HScroll->hide();
        h = m_WindowHeight;
    }

    if (m_HideScrollbars || sHeight <= m_LayoutHeight || (sHeight <= m_WindowHeight && sWidth <= m_WindowWidth))
    {
        m_VScroll->hide();
        w = m_WindowWidth;
    }

    int x = std::max(0, (w - sWidth) / 2),
        y = std::max(0, (h - sHeight) / 2);
    double hAdjustVal, vAdjustVal;

    if (m_ZoomScroll)
    {
        hAdjustVal = m_HAdjust->get_value() / std::max(m_HAdjust->get_upper() - m_HAdjust->get_page_size(), 1.0);
        vAdjustVal = m_VAdjust->get_value() / std::max(m_VAdjust->get_upper() - m_VAdjust->get_page_size(), 1.0);
    }

    m_Layout->set_size(sWidth, sHeight);

    if (temp)
    {
        m_Layout->move(*m_GtkImage, x, y);
        m_GtkImage->set(temp);
    }
    else
    {
        m_Layout->move(*m_DrawingArea, x, y);
        m_DrawingArea->set_size_request(sWidth, sHeight);
    }

    // Reset the scrollbar positions
    if (scroll)
    {
        m_VAdjust->set_value(0);
        if (Settings.get_bool("MangaMode"))
        {
            // Start at the right side of the image
            m_HAdjust->set_value(m_HAdjust->get_upper() - m_HAdjust->get_page_size());
        }
        else
        {
            m_HAdjust->set_value(0);
        }
    }
    else if (m_ZoomScroll)
    {
        m_HAdjust->set_value(std::max(hAdjustVal * (m_HAdjust->get_upper() - m_HAdjust->get_page_size()), 0.0));
        m_VAdjust->set_value(std::max(vAdjustVal * (m_VAdjust->get_upper() - m_VAdjust->get_page_size()), 0.0));
        m_ZoomScroll = false;
    }

    get_window()->thaw_updates();
    m_RedrawQueued = false;

    double scale = m_ZoomMode == ZoomMode::MANUAL ? m_ZoomPercent :
                        static_cast<double>(sWidth) / origWidth * 100;
    m_StatusBar->set_resolution(origWidth, origHeight, scale, m_ZoomMode);

#ifdef HAVE_GSTREAMER
    if (start_playing)
        gst_element_set_state(m_Playbin, GST_STATE_PLAYING);
#endif // HAVE_GSTREAMER
}

bool ImageBox::get_scaled_size(int origWidth, int origHeight, int &w, int &h)
{
    double windowAspect = static_cast<double>(m_WindowWidth) / m_WindowHeight,
           imageAspect  = static_cast<double>(origWidth) / origHeight;

    if ((origWidth > m_WindowWidth || (origHeight > m_WindowHeight && origWidth > m_LayoutWidth)) &&
        (m_ZoomMode == ZoomMode::FIT_WIDTH || (m_ZoomMode == ZoomMode::AUTO_FIT && windowAspect <= imageAspect)))
    {
        w = std::ceil(m_WindowWidth / imageAspect) > m_WindowHeight && !m_HideScrollbars ? m_LayoutWidth : m_WindowWidth;
        h = std::ceil(w / imageAspect);
    }
    else if ((origHeight > m_WindowHeight || (origWidth > m_WindowWidth && origHeight > m_LayoutHeight)) &&
             (m_ZoomMode == ZoomMode::FIT_HEIGHT || (m_ZoomMode == ZoomMode::AUTO_FIT && windowAspect >= imageAspect)))
    {
        h = std::ceil(m_WindowHeight * imageAspect) > m_WindowWidth && !m_HideScrollbars ? m_LayoutHeight : m_WindowHeight;
        w = std::ceil(h * imageAspect);
    }
    else if (m_ZoomMode == ZoomMode::MANUAL && m_ZoomPercent != 100)
    {
        w = origWidth * static_cast<double>(m_ZoomPercent) / 100;
        h = origHeight * static_cast<double>(m_ZoomPercent) / 100;
    }
    else
    {
        // no scaling needed
        w = origWidth;
        h = origHeight;
        return false;
    }

    return true;
}

bool ImageBox::update_animation()
{
    if (m_Image->is_loading())
        return true;

    m_AnimConn.disconnect();
    m_PixbufAnimIter->advance();
    queue_draw_image();

    if (m_PixbufAnimIter->get_delay_time() >= 0)
        m_AnimConn = Glib::signal_timeout().connect(
                sigc::mem_fun(*this, &ImageBox::update_animation),
                m_PixbufAnimIter->get_delay_time());

    return false;
}

void ImageBox::scroll(const int x, const int y, const bool panning, const bool fromSlideshow)
{
    int adjustUpperX = std::max(0, static_cast<int>((m_HAdjust->get_upper()) - m_HAdjust->get_page_size())),
        adjustUpperY = std::max(0, static_cast<int>((m_VAdjust->get_upper()) - m_VAdjust->get_page_size()));

    if (!fromSlideshow)
        reset_slideshow();

    if (panning)
    {
        int nX = m_HAdjust->get_value() + x,
            nY = m_VAdjust->get_value() + y;

        if (nX <= adjustUpperX)
            m_HAdjust->set_value(nX);

        if (nY <= adjustUpperY)
            m_VAdjust->set_value(nY);
    }
    else
    {
        if ((m_HAdjust->get_value() == adjustUpperX && x > 0) ||
            (m_VAdjust->get_value() == adjustUpperY && y > 0))
        {
            m_ScrollConn.disconnect();

            if (m_SlideshowConn && !m_NextAction->is_sensitive())
                m_SlideshowEndedSignal();
            else
                m_NextAction->activate();
        }
        else if ((m_HAdjust->get_value() == 0 && x < 0) ||
                 (m_VAdjust->get_value() == 0 && y < 0))
        {
            m_ScrollConn.disconnect();
            m_PreviousAction->activate();
        }
        else if (x != 0)
        {
            smooth_scroll(x, m_HAdjust);
        }
        else if (y != 0)
        {
            smooth_scroll(y, m_VAdjust);
        }
    }
}
void ImageBox::smooth_scroll(const int amount, const Glib::RefPtr<Gtk::Adjustment> &adj)
{
    // Cancel current animation if we changed direction
    if ((amount < 0 && m_ScrollTarget > 0) ||
        (amount > 0 && m_ScrollTarget < 0) ||
        adj != m_ScrollAdjust || !m_ScrollConn)
    {
        m_ScrollConn.disconnect();
        m_ScrollTarget = m_ScrollDuration = 0;
        m_ScrollAdjust = adj;
    }

    m_ScrollTime = 0;
    m_ScrollTarget += amount;
    m_ScrollStart = m_ScrollAdjust->get_value();

    if (m_ScrollTarget > 0)
    {
        double upperMax = m_ScrollAdjust->get_upper() - m_ScrollAdjust->get_page_size() -
                          m_ScrollAdjust->get_value();
        m_ScrollTarget = std::min(upperMax, m_ScrollTarget);
    }
    else if (m_ScrollTarget < 0)
    {
        double lowerMax = m_ScrollAdjust->get_lower() - m_ScrollAdjust->get_value();
        m_ScrollTarget = std::max(lowerMax, m_ScrollTarget);
    }

    m_ScrollDuration = std::ceil(std::min(500.0, std::abs(m_ScrollTarget)) / SmoothScrollStep) * SmoothScrollStep;

    if (!m_ScrollConn)
        m_ScrollConn = Glib::signal_timeout().connect(
                sigc::mem_fun(*this, &ImageBox::update_smooth_scroll),
                SmoothScrollStep);
}

bool ImageBox::update_smooth_scroll()
{
    m_ScrollTime += SmoothScrollStep;

    double val = m_ScrollTarget * std::sin(m_ScrollTime / m_ScrollDuration * (M_PI / 2)) + m_ScrollStart;
    val = m_ScrollTarget < 0 ? std::floor(val) : std::ceil(val);

    m_ScrollAdjust->set_value(val);

    return m_ScrollAdjust->get_value() != m_ScrollStart + m_ScrollTarget;
}


void ImageBox::zoom(const std::uint32_t percent)
{
    if (m_ZoomMode != ZoomMode::MANUAL || percent < 10 || percent > 400)
        return;

    m_ZoomScroll = m_ZoomPercent != percent;
    m_ZoomPercent = percent;
    queue_draw_image();
}

bool ImageBox::advance_slideshow()
{
    // TODO: Smart scrolling, as an option
    // e.g. Scroll the width of the image before scrolling down
    scroll(0, 300, false, true);

    return true;
}

void ImageBox::cursor_timeout()
{
    // TODO: Maybe put the time in the settings,
    // with 0 = never hide cursor
    m_CursorConn.disconnect();
    m_Layout->get_window()->set_cursor(m_LeftPtrCursor);
    m_CursorConn = Glib::signal_timeout().connect(sigc::bind_return([ this ]()
                { m_Layout->get_window()->set_cursor(m_BlankCursor); }, false), 2000);
}
