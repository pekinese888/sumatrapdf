/* Copyright 2020 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "utils/BaseUtil.h"
#include "utils/Archive.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/DirIter.h"
#include "utils/HtmlParserLookup.h"
#include "utils/HtmlPullParser.h"
#include "utils/TrivialHtmlParser.h"
#include "utils/WinUtil.h"
#include "utils/ZipUtil.h"
#include "utils/Log.h"

#include "AppColors.h"
#include "wingui/TreeModel.h"
#include "EngineBase.h"
#include "EngineFzUtil.h"
#include "EngineManager.h"
#include "ParseBKM.h"
#include "EngineMulti.h"

struct EngineInfo {
    TocItem* tocRoot = nullptr;
    EngineBase* engine = nullptr;
};

struct EnginePage {
    int pageNoInEngine = 0;
    EngineBase* engine = nullptr;
};

Kind kindEngineMulti = "enginePdfMulti";

class EngineMulti : public EngineBase {
  public:
    EngineMulti();
    virtual ~EngineMulti();
    EngineBase* Clone() override;

    RectD PageMediabox(int pageNo) override;
    RectD PageContentBox(int pageNo, RenderTarget target = RenderTarget::View) override;

    RenderedBitmap* RenderPage(RenderPageArgs& args) override;

    RectD Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse = false) override;

    std::string_view GetFileData() override;
    bool SaveFileAs(const char* copyFileName, bool includeUserAnnots = false) override;
    bool SaveFileAsPdf(const char* pdfFileName, bool includeUserAnnots = false);
    WCHAR* ExtractPageText(int pageNo, RectI** coordsOut = nullptr) override;

    bool HasClipOptimizations(int pageNo) override;
    WCHAR* GetProperty(DocumentProperty prop) override;

    void UpdateUserAnnotations(Vec<PageAnnotation>* list) override;

    bool BenchLoadPage(int pageNo) override;

    Vec<PageElement*>* GetElements(int pageNo) override;
    PageElement* GetElementAtPos(int pageNo, PointD pt) override;
    RenderedBitmap* GetImageForPageElement(PageElement*) override;

    PageDestination* GetNamedDest(const WCHAR* name) override;
    TocTree* GetToc() override;

    WCHAR* GetPageLabel(int pageNo) const override;
    int GetPageByLabel(const WCHAR* label) const override;

    bool Load(const WCHAR* fileName, PasswordUI* pwdUI);
    bool LoadFromFiles(std::string_view dir, VecStr& files);
    void UpdatePagesForEngines(Vec<EngineInfo>& enginesInfo);

    EngineBase* PageToEngine(int& pageNo) const;
    VbkmFile vbkm;
    Vec<EnginePage> pageToEngine;

    TocTree* tocTree = nullptr;
};

EngineBase* EngineMulti::PageToEngine(int& pageNo) const {
    EnginePage& ep = pageToEngine[pageNo - 1];
    pageNo = ep.pageNoInEngine;
    return ep.engine;
}

EngineMulti::EngineMulti() {
    kind = kindEngineMulti;
    defaultFileExt = L".vbkm";
    fileDPI = 72.0f;
    supportsAnnotations = false;
    supportsAnnotationsForSaving = false;
}

EngineMulti::~EngineMulti() {
    delete tocTree;
}

EngineBase* EngineMulti::Clone() {
    const WCHAR* fileName = FileName();
    CrashIf(!fileName);
    // TODO: support CreateFromFiles()
    return CreateEngineMultiFromFile(fileName, nullptr);
}

RectD EngineMulti::PageMediabox(int pageNo) {
    EngineBase* e = PageToEngine(pageNo);
    return e->PageMediabox(pageNo);
}

RectD EngineMulti::PageContentBox(int pageNo, RenderTarget target) {
    EngineBase* e = PageToEngine(pageNo);
    return e->PageContentBox(pageNo, target);
}

RenderedBitmap* EngineMulti::RenderPage(RenderPageArgs& args) {
    EngineBase* e = PageToEngine(args.pageNo);
    return e->RenderPage(args);
}

RectD EngineMulti::Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse) {
    EngineBase* e = PageToEngine(pageNo);
    return e->Transform(rect, pageNo, zoom, rotation, inverse);
}

std::string_view EngineMulti::GetFileData() {
    return {};
}

bool EngineMulti::SaveFileAs(const char* copyFileName, bool includeUserAnnots) {
    return false;
}

bool EngineMulti::SaveFileAsPdf(const char* pdfFileName, bool includeUserAnnots) {
    return false;
}

WCHAR* EngineMulti::ExtractPageText(int pageNo, RectI** coordsOut) {
    EngineBase* e = PageToEngine(pageNo);
    return e->ExtractPageText(pageNo, coordsOut);
}

bool EngineMulti::HasClipOptimizations(int pageNo) {
    EngineBase* e = PageToEngine(pageNo);
    return e->HasClipOptimizations(pageNo);
}

WCHAR* EngineMulti::GetProperty(DocumentProperty prop) {
    return nullptr;
}

void EngineMulti::UpdateUserAnnotations(Vec<PageAnnotation>* list) {
    // TODO: support user annotations
}

bool EngineMulti::BenchLoadPage(int pageNo) {
    EngineBase* e = PageToEngine(pageNo);
    return e->BenchLoadPage(pageNo);
}

Vec<PageElement*>* EngineMulti::GetElements(int pageNo) {
    EngineBase* e = PageToEngine(pageNo);
    return e->GetElements(pageNo);
}

PageElement* EngineMulti::GetElementAtPos(int pageNo, PointD pt) {
    EngineBase* e = PageToEngine(pageNo);
    return e->GetElementAtPos(pageNo, pt);
}

RenderedBitmap* EngineMulti::GetImageForPageElement(PageElement* pel) {
    EngineBase* e = PageToEngine(pel->pageNo);
    return e->GetImageForPageElement(pel);
}

PageDestination* EngineMulti::GetNamedDest(const WCHAR* name) {
    for (auto&& pe : pageToEngine) {
        EngineBase* e = pe.engine;
        auto dest = e->GetNamedDest(name);
        if (dest) {
            // TODO: fix up page number in returned destination
            return dest;
        }
    }
    return nullptr;
}

static void updateTocItemsPageNo(TocItem* ti, int nPageNoAdd, bool root) {
    if (nPageNoAdd == 0) {
        return;
    }
    if (!ti) {
        return;
    }
    auto curr = ti;
    while (curr) {
        if (curr->dest) {
            curr->dest->pageNo += nPageNoAdd;
        }
        curr->pageNo += nPageNoAdd;

        updateTocItemsPageNo(curr->child, nPageNoAdd, false);
        if (root) {
            return;
        }
        curr = curr->next;
    }
}

TocTree* EngineMulti::GetToc() {
    CrashIf(!tocTree);
    return tocTree;
}

WCHAR* EngineMulti::GetPageLabel(int pageNo) const {
    if (pageNo < 1 || pageNo >= pageCount) {
        return nullptr;
    }

    EngineBase* e = PageToEngine(pageNo);
    return e->GetPageLabel(pageNo);
}

int EngineMulti::GetPageByLabel(const WCHAR* label) const {
    for (auto&& pe : pageToEngine) {
        EngineBase* e = pe.engine;
        int pageNo = e->GetPageByLabel(label);
        if (pageNo != -1) {
            // TODO: fixup page number
            return pageNo;
        }
    }
    return -1;
}

static void CollectTocItemsRecur(TocItem* ti, Vec<TocItem*>& v) {
    while (ti) {
        v.Append(ti);
        CollectTocItemsRecur(ti->child, v);
        ti = ti->next;
    }
}

static bool cmpByPageNo(TocItem* ti1, TocItem* ti2) {
    return ti1->pageNo < ti2->pageNo;
}

void CalcEndPageNo(TocItem* root, int nPages) {
    Vec<TocItem*> tocItems;
    CollectTocItemsRecur(root, tocItems);
    size_t n = tocItems.size();
    if (n < 1) {
        return;
    }
    std::sort(tocItems.begin(), tocItems.end(), cmpByPageNo);
    TocItem* prev = tocItems[0];
    for (size_t i = 1; i < n; i++) {
        TocItem* next = tocItems[i];
        prev->endPageNo = next->pageNo - 1;
        if (prev->endPageNo < prev->pageNo) {
            prev->endPageNo = prev->pageNo;
        }
        prev = next;
    }
    prev->endPageNo = nPages;
}

#if 0
static void MarkAsInvisibleRecur(TocItem* ti, bool markInvisible, Vec<bool>& visible) {
    while (ti) {
        if (markInvisible) {
            for (int i = ti->pageNo; i < ti->endPageNo; i++) {
                visible[i - 1] = false;
            }
        }
        bool childMarkInvisible = markInvisible;
        if (!childMarkInvisible) {
            childMarkInvisible = ti->isUnchecked;
        }
        MarkAsInvisibleRecur(ti->child, childMarkInvisible, visible);
        ti = ti->next;
    }
}

static void MarkAsVisibleRecur(TocItem* ti, bool markVisible, Vec<bool>& visible) {
    if (!markVisible) {
        return;
    }
    while (ti) {
        for (int i = ti->pageNo; i < ti->endPageNo; i++) {
            visible[i - 1] = true;
        }
        MarkAsInvisibleRecur(ti->child, ti->isUnchecked, visible);
        ti = ti->next;
    }
}

static void CalcRemovedPages(TocItem* root, Vec<bool>& visible) {
    int nPages = (int)visible.size();
    CalcEndPageNo(root, nPages);
    // in the first pass we mark the pages under unchecked nodes as invisible
    MarkAsInvisibleRecur(root, root->isUnchecked, visible);

    // in the second pass we mark back pages that are visible
    // from nodes that are not unchecked
    MarkAsVisibleRecur(root, !root->isUnchecked, visible);
}
#endif

// to supporting moving .vbkm and it's associated files, we accept absolute paths
// and relative to directory of .vbkm file
std::string_view FindEnginePath(std::string_view vbkmPath, std::string_view engineFilePath) {
    if (file::Exists(engineFilePath)) {
        return str::Dup(engineFilePath);
    }
    AutoFreeStr dir = path::GetDir(vbkmPath);
    const char* engineFileName = path::GetBaseNameNoFree(engineFilePath.data());
    AutoFreeStr path = path::JoinUtf(dir, engineFileName, nullptr);
    if (file::Exists(path.as_view())) {
        std::string_view res = path.release();
        return res;
    }
    return {};
}

TocItem* CreateWrapperItem(EngineBase* engine) {
    TocItem* tocFileRoot = nullptr;
    TocTree* tocTree = engine->GetToc();
    // it's ok if engine doesn't have toc
    if (tocTree) {
        tocFileRoot = CloneTocItemRecur(tocTree->root, false);
    }

    int nPages = engine->PageCount();
    const WCHAR* title = path::GetBaseNameNoFree(engine->FileName());
    TocItem* tocWrapper = new TocItem(tocFileRoot, title, 0);
    tocWrapper->isOpenDefault = true;
    tocWrapper->child = tocFileRoot;
    char* filePath = (char*)strconv::WstrToUtf8(engine->FileName()).data();
    tocWrapper->engineFilePath = filePath;
    tocWrapper->nPages = nPages;
    tocWrapper->pageNo = 1;
    if (tocFileRoot) {
        tocFileRoot->parent = tocWrapper;
    }
    return tocWrapper;
}

bool EngineMulti::LoadFromFiles(std::string_view dir, VecStr& files) {
    int n = files.size();
    TocItem* tocFiles = nullptr;
    Vec<EngineInfo> enginesInfo;
    for (int i = 0; i < n; i++) {
        std::string_view path = files.at(i);
        AutoFreeWstr pathW = strconv::Utf8ToWstr(path);
        EngineBase* engine = EngineManager::CreateEngine(pathW);
        if (!engine) {
            continue;
        }

        TocItem* wrapper = CreateWrapperItem(engine);
        if (tocFiles == nullptr) {
            tocFiles = wrapper;
        } else {
            tocFiles->AddSiblingAtEnd(wrapper);
        }

        EngineInfo ei;
        ei.engine = engine;
        ei.tocRoot = wrapper;
        enginesInfo.Append(ei);
    }
    if (tocFiles == nullptr) {
        return false;
    }
    UpdatePagesForEngines(enginesInfo);

    AutoFreeWstr dirW = strconv::Utf8ToWstr(dir);
    TocItem* root = new TocItem(nullptr, dirW, 0);
    root->child = tocFiles;
    tocTree = new TocTree(root);

    AutoFreeWstr fileName = strconv::Utf8ToWstr(dir);
    SetFileName(fileName);

    return true;
}

void EngineMulti::UpdatePagesForEngines(Vec<EngineInfo>& enginesInfo) {
    int nTotalPages = 0;
    for (auto&& ei : enginesInfo) {
        TocItem* root = ei.tocRoot;
        if (root->isUnchecked) {
            continue;
        }
        int nPages = ei.engine->PageCount();
#if 0
        Vec<bool> visiblePages;
        for (int i = 0; i < nPages; i++) {
            visiblePages.Append(true);
        }
        CalcRemovedPages(child, visiblePages);
        int nPage = 0;
        for (int i = 0; i < nPages; i++) {
            if (!visiblePages[i]) {
                continue;
            }
            EnginePage ep{i + 1, ei.engine};
            pageToEngine.Append(ep);
            nPage++;
        }
        updateTocItemsPageNo(child, nTotalPages);
        nTotalPages += nPage;
#else
        for (int i = 1; i <= nPages; i++) {
            EnginePage ep{i, ei.engine};
            pageToEngine.Append(ep);
        }
        updateTocItemsPageNo(ei.tocRoot, nTotalPages, true);
        nTotalPages += nPages;
#endif
    }
    pageCount = nTotalPages;
    CrashIf((size_t)pageCount != pageToEngine.size());

    auto verifyPages = [&nTotalPages](TocItem* ti) -> bool {
        int pageNo = ti->pageNo;
        CrashIf(pageNo > nTotalPages);
        return true;
    };

    for (auto&& ei : enginesInfo) {
        TocItem* root = ei.tocRoot;
        if (root->isUnchecked) {
            continue;
        }
        VisitTocTree(root, verifyPages);
    }
}

bool EngineMulti::Load(const WCHAR* fileName, PasswordUI* pwdUI) {
    AutoFreeStr filePath = strconv::WstrToUtf8(fileName);
    bool ok = LoadVbkmFile(filePath.get(), vbkm);
    if (!ok) {
        return false;
    }

    Vec<EngineInfo> enginesInfo;
    TocItem* tocRoot = CloneTocItemRecur(vbkm.tree->root, true);
    delete vbkm.tree;
    vbkm.tree = nullptr;

    // load all referenced files
    auto loadEngines = [&enginesInfo, &filePath](TocItem* ti) -> bool {
        if (ti->engineFilePath == nullptr) {
            return true;
        }
        if (ti->isUnchecked) {
            return true;
        }

        EngineBase* engine = nullptr;
        AutoFreeStr path = FindEnginePath(filePath.as_view(), ti->engineFilePath);
        if (path.empty()) {
            return false;
        }
        AutoFreeWstr pathW = strconv::Utf8ToWstr(path.as_view());
        engine = EngineManager::CreateEngine(pathW, nullptr);
        if (!engine) {
            return false;
        }
        EngineInfo ei;
        ei.engine = engine;
        ei.tocRoot = ti;
        enginesInfo.Append(ei);
        return true;
    };

    ok = VisitTocTree(tocRoot, loadEngines);
    if (!ok) {
        delete tocRoot;
        return false;
    }

    UpdatePagesForEngines(enginesInfo);
    tocTree = new TocTree(tocRoot);
    SetFileName(fileName);
    return true;
}

bool IsEngineMultiSupportedFile(const WCHAR* fileName, bool sniff) {
    if (sniff) {
        // we don't support sniffing
        return false;
    }
    if (str::EndsWithI(fileName, L".vbkm")) {
        return true;
    }
    // for 'Open Folder' functionality
    if (path::IsDirectory(fileName)) {
        return true;
    }
    return false;
}

EngineBase* CreateEngineMultiFromFile(const WCHAR* fileName, PasswordUI* pwdUI) {
    if (str::IsEmpty(fileName)) {
        return nullptr;
    }
    if (path::IsDirectory(fileName)) {
        return CreateEngineMultiFromDirectory(fileName);
    }
    EngineMulti* engine = new EngineMulti();
    if (!engine->Load(fileName, pwdUI)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

EngineBase* CreateEngineMultiFromFiles(std::string_view dir, VecStr& files) {
    EngineMulti* engine = new EngineMulti();
    if (!engine->LoadFromFiles(dir, files)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

EngineBase* CreateEngineMultiFromDirectory(const WCHAR* dirW) {
    auto isValidFunc = [](std::string_view path) -> bool {
        bool isValid = str::EndsWithI(path.data(), ".pdf");
        return isValid;
    };
    VecStr files;
    AutoFreeStr dir = strconv::WstrToUtf8(dirW);
    bool ok = CollectFilesFromDirectory(dir.as_view(), files, isValidFunc);
    if (!ok) {
        // TODO: show error message
        return nullptr;
    }
    if (files.size() == 0) {
        // TODO: show error message
        return nullptr;
    }
    EngineBase* engine = CreateEngineMultiFromFiles(dir.as_view(), files);
    if (!engine) {
        // TODO: show error message
        return nullptr;
    }
    return engine;
}
