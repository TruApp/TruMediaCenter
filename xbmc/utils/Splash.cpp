/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "system.h"
#include "Splash.h"
#include "guilib/GUIImage.h"
#include "guilib/GUILabelControl.h"
#include "guilib/GUIFontManager.h"
#include "filesystem/File.h"
#include "windowing/WindowingFactory.h"
#include "log.h"

using namespace XFILE;

CSplash::CSplash()
  : m_image(nullptr)
{
  m_messageLayout = NULL;
  m_image = NULL;
  m_layoutWasLoading = false;
}

CSplash::~CSplash()
{
  delete m_image;
  delete m_messageLayout;
}

CSplash& CSplash::GetInstance()
{
  static CSplash instance;
  return instance;
}

void CSplash::Show()
{
  Show("");
}

void CSplash::Show(const std::string& message)
{
  if (!m_image)
  {
    std::string splashImage = "special://home/media/Splash.png";
    if (!XFILE::CFile::Exists(splashImage))
      splashImage = "special://xbmc/media/Splash.png";

    m_image = new CGUIImage(0, 0, 0, 0, g_graphicsContext.GetWidth(), g_graphicsContext.GetHeight(), CTextureInfo(splashImage));
    m_image->SetAspectRatio(CAspectRatio::AR_SCALE);
  }

  g_graphicsContext.Lock();
  g_graphicsContext.Clear();

  RESOLUTION_INFO res = g_graphicsContext.GetResInfo();
  g_graphicsContext.SetRenderingResolution(res, true);

  //render splash image
  g_Windowing.BeginRender();

  m_image->AllocResources();
  m_image->Render();
  m_image->FreeResources();

  // render message
  if (!message.empty())
  {
    if (!m_layoutWasLoading)
    {
      // load arial font, white body, no shadow, size: 20, no additional styling
      CGUIFont *messageFont = g_fontManager.LoadTTF("__splash__", "arial.ttf", 0xFFFFFFFF, 0, 20, FONT_STYLE_NORMAL, false, 1.0f, 1.0f, &res);
      if (messageFont)
        m_messageLayout = new CGUITextLayout(messageFont, true, 0);
      m_layoutWasLoading = true;
    }
    if (m_messageLayout)
    {
      m_messageLayout->Update(message, 1150, false, true);
      float textWidth, textHeight;
      m_messageLayout->GetTextExtent(textWidth, textHeight);
      
      int width = g_graphicsContext.GetWidth();
      int height = g_graphicsContext.GetHeight();
      
      // ideally place text in center of empty area below splash image (Logo to bottom edge 387px @ 1080 height)
      float scale = (float)1080/height;
      float y = height - (387/(scale*2)) - textHeight/2;
      m_messageLayout->RenderOutline(width/2, y, 0, 0xFF000000, XBFONT_CENTER_X, width);
    }
  }
  //show it on screen
  g_Windowing.EndRender();
  CDirtyRegionList dirty;
  g_graphicsContext.Flip(dirty);
  g_graphicsContext.Unlock();
}
