/*************************************************************************
** PsSpecialHandler.cpp                                                 **
**                                                                      **
** This file is part of dvisvgm -- a fast DVI to SVG converter          **
** Copyright (C) 2005-2020 Martin Gieseking <martin.gieseking@uos.de>   **
**                                                                      **
** This program is free software; you can redistribute it and/or        **
** modify it under the terms of the GNU General Public License as       **
** published by the Free Software Foundation; either version 3 of       **
** the License, or (at your option) any later version.                  **
**                                                                      **
** This program is distributed in the hope that it will be useful, but  **
** WITHOUT ANY WARRANTY; without even the implied warranty of           **
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the         **
** GNU General Public License for more details.                         **
**                                                                      **
** You should have received a copy of the GNU General Public License    **
** along with this program; if not, see <http://www.gnu.org/licenses/>. **
*************************************************************************/

#if defined(MIKTEX)
#  include <config.h>
#endif
#include <array>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include "FileFinder.hpp"
#include "FilePath.hpp"
#include "FileSystem.hpp"
#include "Message.hpp"
#include "PathClipper.hpp"
#include "PSPattern.hpp"
#include "PSPreviewFilter.hpp"
#include "PsSpecialHandler.hpp"
#include "SpecialActions.hpp"
#include "SVGTree.hpp"
#include "TensorProductPatch.hpp"
#include "TriangularPatch.hpp"
#include "utility.hpp"
#if defined(MIKTEX_WINDOWS)
#include <miktex/Util/PathNameUtil>
#define EXPATH_(x) MiKTeX::Util::PathNameUtil::ToLengthExtendedPathName(x)
#endif

using namespace std;


bool PsSpecialHandler::COMPUTE_CLIPPATHS_INTERSECTIONS = false;
bool PsSpecialHandler::SHADING_SEGMENT_OVERLAP = false;
int PsSpecialHandler::SHADING_SEGMENT_SIZE = 20;
double PsSpecialHandler::SHADING_SIMPLIFY_DELTA = 0.01;
string PsSpecialHandler::BITMAP_FORMAT;


PsSpecialHandler::PsSpecialHandler () : _psi(this), _previewFilter(_psi)
{
	_psi.setImageDevice(BITMAP_FORMAT);
}


PsSpecialHandler::~PsSpecialHandler () {
	_psi.setActions(nullptr);     // ensure no further PS actions are performed
}


/** Initializes the PostScript handler. It's called by the first use of process(). The
 *  deferred initialization speeds up the conversion of DVI files that doesn't contain
 *  PS specials. */
void PsSpecialHandler::initialize () {
	if (_psSection == PS_NONE) {
		initgraphics();
		// execute dvips prologue/header files
		for (const char *fname : {"tex.pro", "texps.pro", "special.pro", "color.pro"})
			processHeaderFile(fname);
		// disable bop/eop operators to prevent side-effects by
		// unexpected bops/eops present in PS specials
		_psi.execute("\nTeXDict begin /bop{pop pop}def /eop{}def end ");
		_psSection = PS_HEADERS;  // allow to process header specials now
	}
}


/** Set initial values of PS graphics state (see PS language reference (Red Book)), p. 612). */
void PsSpecialHandler::initgraphics () {
	_linewidth = 1;
	_linecap = _linejoin = 0;  // butt end caps and miter joins
	_miterlimit = 4;
	_xmlnode = _savenode = nullptr;
	_isshapealpha = false;               // opacity operators change constant component by default
	_fillalpha = _strokealpha = {1, 1};  // set constant and shape opacity to non-transparent
	_blendmode = 0;   // "normal" mode (no blending)
	_sx = _sy = _cos = 1.0;
	_pattern = nullptr;
	_patternEnabled = false;
	_currentcolor = Color::BLACK;
	_dashoffset = 0;
	_dashpattern.clear();
	_path.clear();
	_clipStack.clear();
}


void PsSpecialHandler::processHeaderFile (const char *name) {
	if (const char *path = FileFinder::instance().lookup(name, false)) {
#if defined(MIKTEX_WINDOWS)
                ifstream ifs(EXPATH_(path));
#else
		ifstream ifs(path);
#endif
		_psi.execute(string("%%BeginProcSet: ")+name+" 0 0\n", false);
		_psi.execute(ifs, false);
		_psi.execute("%%EndProcSet\n", false);
	}
	else
		Message::wstream(true) << "PostScript header file " << name << " not found\n";
}


void PsSpecialHandler::enterBodySection () {
	if (_psSection == PS_HEADERS) {
		_psSection = PS_BODY; // don't process any PS header code
		ostringstream oss;
		// process collected header code
		if (!_headerCode.empty()) {
			oss << "\nTeXDict begin @defspecial " << _headerCode << "\n@fedspecial end";
			_headerCode.clear();
		}
		// push dictionary "TeXDict" with dvips definitions on dictionary stack
		// and initialize basic dvips PostScript variables
		oss << "\nTeXDict begin 0 0 1000 72 72 () @start 0 0 moveto ";
		_psi.execute(oss.str(), false);
		// Check for information generated by preview.sty. If the tightpage options
		// was set, don't execute the bop-hook but allow the PS filter to read
		// the bbox data present at the beginning of the page.
		_psi.setFilter(&_previewFilter);
		_previewFilter.activate();
		if (!_previewFilter.tightpage())
			_psi.execute("userdict/bop-hook known{bop-hook}if\n", false);
	}
}


/** Move PS graphic position to current DVI location. */
void PsSpecialHandler::moveToDVIPos () {
	if (_actions) {
		const double x = _actions->getX();
		const double y = _actions->getY();
		ostringstream oss;
		oss << '\n' << x << ' ' << y << " moveto ";
		_psi.execute(oss.str());
		_currentpoint = DPair(x, y);
	}
}


/** Executes a PS snippet and optionally synchronizes the DVI cursor position
 *  with the current PS point.
 *  @param[in] is  stream to read the PS code from
 *  @param[in] updatePos if true, move the DVI drawing position to the current PS point */
void PsSpecialHandler::executeAndSync (istream &is, bool updatePos) {
	if (_actions && _actions->getColor() != _currentcolor) {
		// update the PS graphics state if the color has been changed by a color special
		double r, g, b;
		_actions->getColor().getRGB(r, g, b);
		ostringstream oss;
		oss << '\n' << r << ' ' << g << ' ' << b << " setrgbcolor ";
		_psi.execute(oss.str(), false);
	}
	_psi.execute(is);
	if (updatePos) {
		// retrieve current PS position (stored in _currentpoint)
		_psi.execute("\nquerypos ");
		if (_actions) {
			_actions->setX(_currentpoint.x());
			_actions->setY(_currentpoint.y());
		}
	}
}


void PsSpecialHandler::preprocess (const string &prefix, istream &is, SpecialActions &actions) {
	initialize();
	if (_psSection != PS_HEADERS)
		return;

	_actions = &actions;
	if (prefix == "!") {
		_headerCode += "\n";
		_headerCode += string(istreambuf_iterator<char>(is), istreambuf_iterator<char>());
	}
	else if (prefix == "header=") {
		// read and execute PS header file
		string fname;
		is >> fname;
		processHeaderFile(fname.c_str());
	}
}


static string filename_suffix (const string &fname) {
	string ret;
	size_t pos = fname.rfind('.');
	if (pos != string::npos)
		ret = util::tolower(fname.substr(pos+1));
	return ret;
}


bool PsSpecialHandler::process (const string &prefix, istream &is, SpecialActions &actions) {
	// process PS headers only once (in prescan)
	if (prefix == "!" || prefix == "header=")
		return true;

	_actions = &actions;
	initialize();
	if (_psSection != PS_BODY)
		enterBodySection();

	if (prefix == "\"" || prefix == "pst:") {
		// read and execute literal PostScript code (isolated by a wrapping save/restore pair)
		moveToDVIPos();
		_psi.execute("\n@beginspecial @setspecial ");
		executeAndSync(is, false);
		_psi.execute("\n@endspecial ");
	}
	else if (prefix == "psfile=" || prefix == "PSfile=" || prefix == "pdffile=") {
		if (_actions) {
			StreamInputReader in(is);
			string fname = in.getQuotedString(in.peek() == '"' ? "\"" : nullptr);
			fname = FileSystem::ensureForwardSlashes(fname);
			FileType fileType = FileType::EPS;
			if (prefix == "pdffile")
				fileType = FileType::PDF;
			else {
				// accept selected non-PS files in psfile special
				string ext = filename_suffix(fname);
				if (ext == "pdf")
					fileType = FileType::PDF;
				else if (ext == "svg")
					fileType = FileType::SVG;
				else if (ext == "jpg" || ext == "jpeg" || ext == "png")
					fileType = FileType::BITMAP;
			}
			map<string,string> attr;
			in.parseAttributes(attr, false);
			imgfile(fileType, fname, attr);
		}
	}
	else if (prefix == "ps::") {
		if (_actions)
			_actions->finishLine();  // reset DVI position on next DVI command
		if (is.peek() == '[') {
			// collect characters inside the brackets
			string code;
			for (int i=0; i < 9 && is.peek() != ']' && !is.eof(); ++i)
				code += char(is.get());
			if (is.peek() == ']')
				code += char(is.get());

			if (code == "[begin]" || code == "[nobreak]") {
				moveToDVIPos();
				executeAndSync(is, true);
			}
			else {
				// no move to DVI position here
				if (code != "[end]") // PS array?
					_psi.execute(code);
				executeAndSync(is, true);
			}
		}
		else { // ps::<code> behaves like ps::[end]<code>
			// no move to DVI position here
			executeAndSync(is, true);
		}
	}
	else { // ps: ... or PST: ...
		if (_actions)
			_actions->finishLine();
		moveToDVIPos();
		StreamInputReader in(is);
		if (in.check(" plotfile ")) { // ps: plotfile fname
			string fname = in.getString();
#if defined(MIKTEX_WINDOWS)
                        ifstream ifs(EXPATH_(fname));
#else
			ifstream ifs(fname);
#endif
			if (ifs)
				_psi.execute(ifs);
			else
				Message::wstream(true) << "file '" << fname << "' not found in ps: plotfile\n";
		}
		else {
			// ps:<code> is almost identical to ps::[begin]<code> but does
			// a final repositioning to the current DVI location
			executeAndSync(is, true);
			moveToDVIPos();
		}
	}
	return true;
}


/** Handles a psfile/pdffile special which places an external EPS/PDF graphic
 *  at the current DVI position. The lower left corner (llx,lly) of the
 *  given bounding box is placed at the DVI position.
 *  @param[in] filetype type of file to process (EPS or PDF)
 *  @param[in] fname EPS/PDF file to be included
 *  @param[in] attr attributes given with psfile/pdffile special */
void PsSpecialHandler::imgfile (FileType filetype, const string &fname, const map<string,string> &attr) {
	// prevent warning about missing image file "/dev/null" which is
	// added by option "psfixbb" of the preview package
	if (fname == "/dev/null")
		return;

	map<string,string>::const_iterator it;

	// bounding box of EPS figure in PS point units (lower left and upper right corner)
	double llx = (it = attr.find("llx")) != attr.end() ? stod(it->second) : 0;
	double lly = (it = attr.find("lly")) != attr.end() ? stod(it->second) : 0;
	double urx = (it = attr.find("urx")) != attr.end() ? stod(it->second) : 0;
	double ury = (it = attr.find("ury")) != attr.end() ? stod(it->second) : 0;
	int pageno = (it = attr.find("page")) != attr.end() ? stoi(it->second, nullptr, 10) : 1;

	if (filetype == FileType::BITMAP || filetype == FileType::SVG)
		swap(lly, ury);
	else if (filetype == FileType::PDF && llx == 0 && lly == 0 && urx == 0 && ury == 0) {
		BoundingBox pagebox = _psi.pdfPageBox(fname, pageno);
		if (pagebox.valid()) {
			llx = pagebox.minX();
			lly = pagebox.minY();
			urx = pagebox.maxX();
			ury = pagebox.maxY();
		}
	}

	// desired width and height of the untransformed figure in PS point units
	double rwi = (it = attr.find("rwi")) != attr.end() ? stod(it->second)/10.0 : -1;
	double rhi = (it = attr.find("rhi")) != attr.end() ? stod(it->second)/10.0 : -1;
	if (rwi == 0 || rhi == 0 || urx-llx == 0 || ury-lly == 0)
		return;

	// user transformations (default values chosen according to dvips manual)
	// order of transformations: rotate, scale, translate/offset
	double hoffset = (it = attr.find("hoffset")) != attr.end() ? stod(it->second) : 0;
	double voffset = (it = attr.find("voffset")) != attr.end() ? stod(it->second) : 0;
//	double hsize   = (it = attr.find("hsize")) != attr.end() ? stod(it->second) : 612;
//	double vsize   = (it = attr.find("vsize")) != attr.end() ? stod(it->second) : 792;
	double hscale  = (it = attr.find("hscale")) != attr.end() ? stod(it->second) : 100;
	double vscale  = (it = attr.find("vscale")) != attr.end() ? stod(it->second) : 100;
	double angle   = (it = attr.find("angle")) != attr.end() ? stod(it->second) : 0;

	bool clipToBbox = (attr.find("clip") != attr.end());

	// compute factors to scale the bounding box to width rwi and height rhi
	double sx = rwi/abs(llx-urx);
	double sy = rhi/abs(lly-ury);
	if (sx == 0 || sy == 0)
		return;

	if (sx < 0) sx = sy;         // rwi attribute not set
	if (sy < 0) sy = sx;         // rhi attribute not set
	if (sx < 0) sx = sy = 1.0;   // neither rwi nor rhi set

	// save current DVI position
	double x = _actions->getX();
	double y = _actions->getY();

	// all following drawings are relative to (0,0)
	_actions->setX(0);
	_actions->setY(0);
	moveToDVIPos();

	auto imgNode = createImageNode(filetype, fname, pageno, BoundingBox(llx, lly, urx, ury), clipToBbox);
	if (imgNode) {  // has anything been drawn?
		Matrix matrix(1);
		if (filetype == FileType::EPS || filetype == FileType::PDF)
			sy = -sy;  // adapt orientation of y-coordinates
		matrix.scale(sx, sy).rotate(-angle).scale(hscale/100, vscale/100);  // apply transformation attributes
		matrix.translate(x+hoffset, y-voffset);     // move image to current DVI position
		matrix.lmultiply(_actions->getMatrix());

		// update bounding box
		BoundingBox bbox(0, 0, urx-llx, ury-lly);
		bbox.transform(matrix);
		_actions->embed(bbox);

		// insert element containing the image data
		matrix.rmultiply(TranslationMatrix(-llx, -lly));  // move lower left corner of image to origin
		if (!matrix.isIdentity())
			imgNode->addAttribute("transform", matrix.toSVG());
		_actions->svgTree().appendToPage(std::move(imgNode));
	}
	// restore DVI position
	_actions->setX(x);
	_actions->setY(y);
	moveToDVIPos();
}


/** Returns path + basename of temporary bitmap images. */
static string image_base_path (SpecialActions &actions) {
	FilePath imgpath = actions.getSVGFilePath(actions.getCurrentPageNumber());
	return FileSystem::tmpdir() + "/" + imgpath.basename() + "-tmp-";
}


/** Creates an XML element containing the image data depending on the file type.
 *  @param[in] type file type of the image
 *  @param[in] fname file name/path of image file
 *  @param[in] pageno number of page to process (PDF only)
 *  @param[in] bbox bounding box of the image
 *  @param[in] clip if true, the image is clipped to its bounding box
 *  @return pointer to the element or nullptr if there's no image data */
unique_ptr<XMLElement> PsSpecialHandler::createImageNode (FileType type, const string &fname, int pageno, BoundingBox bbox, bool clip) {
	unique_ptr<XMLElement> node;
	string pathstr;
	if (const char *path = FileFinder::instance().lookup(fname, false))
		pathstr = FileSystem::ensureForwardSlashes(path);
	if ((pathstr.empty() || !FileSystem::exists(pathstr)) && FileSystem::exists(fname))
		pathstr = fname;
	if (pathstr.empty())
		Message::wstream(true) << "file '" << fname << "' not found\n";
	else if (type == FileType::BITMAP || type == FileType::SVG) {
		node = util::make_unique<XMLElement>("image");
		node->addAttribute("x", 0);
		node->addAttribute("y", 0);
		node->addAttribute("width", bbox.width());
		node->addAttribute("height", bbox.height());

		// Only reference the image with an absolute path if either an absolute path was given by the user
		// or a given plain filename is not present in the current working directory but was found through
		// the FileFinder, i.e. it's usually located somewhere in the texmf tree.
		string href = pathstr;
		if (!FilePath::isAbsolute(fname) && (fname.find('/') != string::npos || FilePath(fname).exists()))
			href = FilePath(pathstr).relative(FilePath(_actions->getSVGFilePath(pageno)));
		node->addAttribute("xlink:href", href);
	}
	else {  // PostScript or PDF
		// clip image to its bounding box if flag 'clip' is given
		string rectclip;
		if (clip)
			rectclip = to_string(bbox.minX())+" "+to_string(bbox.minY())+" "+to_string(bbox.width())+" "+to_string(bbox.height())+" rectclip";

		node = util::make_unique<XMLElement>("g"); // put SVG nodes created from the EPS/PDF file in this group
		_xmlnode = node.get();
		_psi.execute(
			"\n@beginspecial @setspecial"            // enter special environment
			"/setpagedevice{@setpagedevice}def "     // activate processing of operator "setpagedevice"
			"/@imgbase("+image_base_path(*_actions)+")store " // path and basename of image files
			"matrix setmatrix"                       // don't apply outer PS transformations
			"/FirstPage "+to_string(pageno)+" def"   // set number of first page to convert (PDF only)
			"/LastPage "+to_string(pageno)+" def "   // set number of last page to convert (PDF only)
			+rectclip+                               // clip to bounding box (if requexted by attribute 'clip')
			"(" + pathstr + ")run "                  // execute file content
			"@endspecial\n"                          // leave special environment
		);
		if (node->empty())
			node.reset(nullptr);
		_xmlnode = nullptr;   // append following elements to page group again
	}
	return node;
}


/** Apply transformation to width, height, and depth set by preview package.
 *  @param[in] matrix transformation matrix to apply
 *  @param[out] w width
 *  @param[out] h height
 *  @param[out] d depth
 *  @return true if the baseline is still horizontal after the transformation */
static bool transform_box_extents (const Matrix &matrix, double &w, double &h, double &d) {
	DPair shift = matrix*DPair(0,0);  // the translation component of the matrix
	DPair ex = matrix*DPair(1,0)-shift;
	DPair ey = matrix*DPair(0,1)-shift;
	if (ex.y() != 0 && ey.x() != 0)  // rotation != mod 90 degrees?
		return false;                 // => non-horizontal baseline, can't compute meaningful extents

	if (ex.y() == 0)  // horizontal scaling or skewing?
		w *= abs(ex.x());
	if (ey.x()==0 || ex.y()==0) { // vertical scaling?
		if (ey.y() < 0)
			swap(h, d);
		double sy = abs(ey.y())/ey.length();
		if (sy < 1e-8)
			h = d = 0;
		else {
			h *= abs(ey.y()/sy);
			d *= abs(ey.y()/sy);
		}
	}
	return true;
}


void PsSpecialHandler::dviBeginPage (unsigned int pageno, SpecialActions &actions) {
	_psi.execute("/@imgbase("+image_base_path(actions)+")store\n"); // path and basename of image files
}


void PsSpecialHandler::dviEndPage (unsigned, SpecialActions &actions) {
	BoundingBox bbox;
	if (_previewFilter.getBoundingBox(bbox)) {  // is there any data written by preview package?
		double w=0, h=0, d=0;
		if (actions.getBBoxFormatString() == "preview" || actions.getBBoxFormatString() == "min") {
			if (actions.getBBoxFormatString() == "preview") {
				w = max(0.0, _previewFilter.width());
				h = max(0.0, _previewFilter.height());
				d = max(0.0, _previewFilter.depth());
				actions.bbox() = bbox;
				Message::mstream() << "\napplying bounding box set by";
			}
			else {
				w = actions.bbox().width();
				h = max(0.0, -actions.bbox().minY());
				d = max(0.0, actions.bbox().maxY());
				Message::mstream() << "\ncomputing extents based on data set by";
			}
			Message::mstream() << " preview package (version " << _previewFilter.version() << ")\n";

			// apply page transformations to box extents
			Matrix pagetrans = actions.getPageTransformation();
			bool isBaselineHorizontal = transform_box_extents(pagetrans, w, h, d);
			actions.bbox().lock();

			if (!isBaselineHorizontal)
				Message::mstream() << "can't determine height, width, and depth due to non-horizontal baseline\n";
			else {
				const double bp2pt = 72.27/72.0;
				Message::mstream() <<
					"width=" << XMLString(w*bp2pt) << "pt, "
					"height=" << XMLString(h*bp2pt) << "pt, "
					"depth=" << XMLString(d*bp2pt) << "pt\n";
			}
#if 0
			auto rect = util::make_unique<XMLElement>("rect");
			rect->addAttribute("x", actions.bbox().minX());
			rect->addAttribute("y", actions.bbox().minY());
			rect->addAttribute("width", w);
			rect->addAttribute("height", h+d);
			rect->addAttribute("fill", "none");
			rect->addAttribute("stroke", "red");
			rect->addAttribute("stroke-width", "0.1");
			actions.appendToPage(std::move(rect));
			if (d > 0) {
				auto line = util::make_unique<XMLElement>("line");
				line->addAttribute("x1", actions.bbox().minX());
				line->addAttribute("y1", actions.bbox().minY()+h);
				line->addAttribute("x2", actions.bbox().maxX());
				line->addAttribute("y2", actions.bbox().minY()+h);
				line->addAttribute("stroke", "blue");
				line->addAttribute("stroke-width", "0.1");
				actions.appendToPage(std::move(line));
			}
#endif
		}
	}
	// close dictionary TeXDict and execute end-hook if defined
	if (_psSection == PS_BODY) {
		_psi.execute("\nend userdict/end-hook known{end-hook}if initgraphics ");
		initgraphics();  // reset graphics state to default values
		_psSection = PS_HEADERS;
	}
}

///////////////////////////////////////////////////////

void PsSpecialHandler::setpagedevice (std::vector<double> &p) {
	_linewidth = 1;
	_linecap = _linejoin = 0;  // butt end caps and miter joins
	_miterlimit = 4;
	_isshapealpha = false;               // opacity operators change constant component by default
	_fillalpha = _strokealpha = {1, 1};  // set constant and shape opacity to non-transparent
	_blendmode = 0;  // "normal" mode (no blending)
	_sx = _sy = _cos = 1.0;
	_pattern = nullptr;
	_currentcolor = Color::BLACK;
	_dashoffset = 0;
	_dashpattern.clear();
	_path.clear();
}


void PsSpecialHandler::gsave (vector<double>&) {
	_clipStack.dup();
}


void PsSpecialHandler::grestore (vector<double>&) {
	_clipStack.pop();
}


void PsSpecialHandler::grestoreall (vector<double>&) {
	_clipStack.pop(-1, true);
}


void PsSpecialHandler::save (vector<double> &p) {
	_clipStack.dup(static_cast<int>(p[0]));
}


void PsSpecialHandler::restore (vector<double> &p) {
	_clipStack.pop(static_cast<int>(p[0]));
}


void PsSpecialHandler::moveto (vector<double> &p) {
	_path.moveto(p[0], p[1]);
}


void PsSpecialHandler::lineto (vector<double> &p) {
	_path.lineto(p[0], p[1]);
}


void PsSpecialHandler::curveto (vector<double> &p) {
	_path.cubicto(p[0], p[1], p[2], p[3], p[4], p[5]);
}


void PsSpecialHandler::closepath (vector<double>&) {
	_path.closepath();
}


static string css_blendmode_name (int mode) {
	static const array<const char*,16> modenames = {{
	  "normal",  "multiply",  "screen", "overlay", "soft-light", "hard-light", "color-dodge", "color-burn",
	  "darken", "lighten", "difference", "exclusion", "hue", "saturation", "color", "luminosity"
	}};
	if (mode < 0 || mode > 15)
		return "";
	return modenames[mode];
}


/** Draws the current path recorded by previously executed path commands (moveto, lineto,...).
 *  @param[in] p not used */
void PsSpecialHandler::stroke (vector<double> &p) {
	_path.removeRedundantCommands();
	if ((_path.empty() && !_clipStack.prependedPath()) || !_actions)
		return;

	BoundingBox bbox;
	if (!_actions->getMatrix().isIdentity()) {
		_path.transform(_actions->getMatrix());
		if (!_xmlnode)
			bbox.transform(_actions->getMatrix());
	}
	if (_clipStack.prependedPath())
		_path.prepend(*_clipStack.prependedPath());
	unique_ptr<XMLElement> path;
	Pair<double> point;
	if (_path.isDot(point)) {  // zero-length path?
		if (_linecap == 1) {    // round line ends?  => draw dot
			double x = point.x();
			double y = point.y();
			double r = _linewidth/2.0;
			path = util::make_unique<XMLElement>("circle");
			path->addAttribute("cx", x);
			path->addAttribute("cy", y);
			path->addAttribute("r", r);
			path->addAttribute("fill", _actions->getColor().svgColorString());
			bbox = BoundingBox(x-r, y-r, x+r, y+r);
		}
	}
	else {
		// compute bounding box
		bbox = _path.computeBBox();
		bbox.expand(_linewidth/2);

		ostringstream oss;
		_path.writeSVG(oss, SVGTree::RELATIVE_PATH_CMDS);
		path = util::make_unique<XMLElement>("path");
		path->addAttribute("d", oss.str());
		path->addAttribute("stroke", _actions->getColor().svgColorString());
		path->addAttribute("fill", "none");
		if (_linewidth != 1)
			path->addAttribute("stroke-width", _linewidth);
		if (_miterlimit != 4)
			path->addAttribute("stroke-miterlimit", _miterlimit);
		if (_linecap > 0)     // default value is "butt", no need to set it explicitly
			path->addAttribute("stroke-linecap", _linecap == 1 ? "round" : "square");
		if (_linejoin > 0)    // default value is "miter", no need to set it explicitly
			path->addAttribute("stroke-linejoin", _linecap == 1 ? "round" : "bevel");
		if (_strokealpha[0] < 1 || _strokealpha[1] < 1)
			path->addAttribute("stroke-opacity", _strokealpha[0] * _strokealpha[1]);
		if (_blendmode > 0 && _blendmode < 16)
			path->addAttribute("style", "mix-blend-mode:"+css_blendmode_name(_blendmode));
		if (!_dashpattern.empty()) {
			string patternStr;
			for (double dashValue : _dashpattern)
				patternStr += XMLString(dashValue)+",";
			patternStr.pop_back();
			path->addAttribute("stroke-dasharray", patternStr);
			if (_dashoffset != 0)
				path->addAttribute("stroke-dashoffset", _dashoffset);
		}
	}
	if (path && _clipStack.path() && !_savenode) {
		// assign clipping path and clip bounding box
		path->addAttribute("clip-path", XMLString("url(#clip")+XMLString(_clipStack.topID())+")");
		bbox.intersect(_clipStack.path()->computeBBox());
		_clipStack.removePrependedPath();
	}
	if (_xmlnode)
		_xmlnode->append(std::move(path));
	else {
		_actions->svgTree().appendToPage(std::move(path));
		_actions->embed(bbox);
	}
	_path.clear();
}


/** Draws a closed path filled with the current color.
 *  @param[in] p not used
 *  @param[in] evenodd true: use even-odd fill algorithm, false: use nonzero fill algorithm */
void PsSpecialHandler::fill (vector<double> &p, bool evenodd) {
	_path.removeRedundantCommands();
	if ((_path.empty() && !_clipStack.prependedPath()) || !_actions)
		return;

	// compute bounding box
	BoundingBox bbox = _path.computeBBox();
	if (!_actions->getMatrix().isIdentity()) {
		_path.transform(_actions->getMatrix());
		if (!_xmlnode)
			bbox.transform(_actions->getMatrix());
	}
	if (_clipStack.prependedPath())
		_path.prepend(*_clipStack.prependedPath());

	ostringstream oss;
	_path.writeSVG(oss, SVGTree::RELATIVE_PATH_CMDS);
	unique_ptr<XMLElement> path = util::make_unique<XMLElement>("path");
	path->addAttribute("d", oss.str());
	if (_pattern)
		path->addAttribute("fill", XMLString("url(#")+_pattern->svgID()+")");
	else if (_actions->getColor() != Color::BLACK || _savenode)
		path->addAttribute("fill", _actions->getColor().svgColorString());
	if (_clipStack.path() && !_savenode) {  // clip path active and not inside pattern definition?
		// assign clipping path and clip bounding box
		path->addAttribute("clip-path", XMLString("url(#clip")+XMLString(_clipStack.topID())+")");
		bbox.intersect(_clipStack.path()->computeBBox());
		_clipStack.removePrependedPath();
	}
	if (evenodd)  // SVG default fill rule is "nonzero" algorithm
		path->addAttribute("fill-rule", "evenodd");
	if (_fillalpha[0] < 1 || _fillalpha[1] < 1)
		path->addAttribute("fill-opacity", _fillalpha[0] * _fillalpha[1]);
	if (_blendmode > 0 && _blendmode < 16)
		path->addAttribute("style", "mix-blend-mode:"+css_blendmode_name(_blendmode));
	if (_xmlnode)
		_xmlnode->append(std::move(path));
	else {
		_actions->svgTree().appendToPage(std::move(path));
		_actions->embed(bbox);
	}
	_path.clear();
}


/** Postprocesses the 'image' operation performed by the PS interpreter. If
 *  the PS image operator succeeded, there's now a PNG file that must be embedded
 *  into the SVG file. */
void PsSpecialHandler::image (std::vector<double> &p) {
	int imgID = static_cast<int>(p[0]);   // ID of PNG file written
	if (imgID < 0)  // no bitmap file written?
		return;

	double width = p[1];
	double height = p[2];
	string suffix = (BITMAP_FORMAT.substr(0, 3) == "png" ? ".png" : ".jpg");
	string fname = image_base_path(*_actions)+to_string(imgID)+suffix;
#if defined(MIKTEX_WINDOWS)
	ifstream ifs(EXPATH_(fname), ios::binary);
#else
	ifstream ifs(fname, ios::binary);
#endif
	if (ifs) {
		ifs.close();
		auto image = util::make_unique<XMLElement>("image");
		double x = _actions->getX();
		double y = _actions->getY();
		image->addAttribute("x", x);
		image->addAttribute("y", y);
		image->addAttribute("width", util::to_string(width));
		image->addAttribute("height", util::to_string(height));

		// The current transformation matrix (CTM) maps the unit square to the rectangular region
		// of the target canvas showing the bitmap (see PS Reference Manual, 4.10.3). Therefore,
		// the local pixel coordinates of the original bitmap must be transformed by CTM*inv(M) to
		// get the target coordinates. M is the matrix that maps the unit square to the bitmap rectangle.
		Matrix matrix{width, 0, 0, 0, -height, height};  // maps unit square to bitmap rectangle
		matrix = matrix.invert().lmultiply(_actions->getMatrix());
		image->addAttribute("transform", matrix.toSVG());

		// To prevent memory issues, only add the filename to the href attribute and tag it by '@'
		// for later base64 encoding.
		image->addAttribute("@xlink:href", string("data:image/")+(suffix == ".png" ? "png" : "jpeg")+";base64,"+fname);

		// if set, assign clipping path to image
		if (_clipStack.path()) {
			auto group = util::make_unique<XMLElement>("g");
			group->addAttribute("clip-path", XMLString("url(#clip")+XMLString(_clipStack.topID())+")");
			group->append(std::move(image));
			image = std::move(group);  // handle the entire group as image to add
		}
		if (_xmlnode)
			_xmlnode->append(std::move(image));
		else {
			_actions->svgTree().appendToPage(std::move(image));
			BoundingBox bbox(x, y, x+width, y+height);
			bbox.transform(matrix);
			if (_clipStack.path())
				bbox.intersect(_clipStack.path()->computeBBox());
			_actions->embed(bbox);
		}
	}
}


/** Creates a Matrix object from a given sequence of 6 double values.
 *  The given values must be arranged in PostScript matrix order.
 *  @param[in] v vector containing the matrix values
 *  @param[in] startindex vector index of first component
 *  @param[out] matrix the generated matrix */
static void create_matrix (vector<double> &v, int startindex, Matrix &matrix) {
	// Ensure vector p has 6 elements. If necessary, add missing ones
	// using corresponding values of the identity matrix.
	if (v.size()-startindex < 6) {
		v.resize(6+startindex);
		for (size_t i=v.size()-startindex; i < 6; ++i)
			v[i+startindex] = (i%3 ? 0 : 1);
	}
	// PS matrix [a b c d e f] equals ((a,b,0),(c,d,0),(e,f,1)).
	// Since PS uses left multiplications, we must transpose and reorder
	// the matrix to ((a,c,e),(b,d,f),(0,0,1)). This is done by the
	// following swaps.
	swap(v[startindex+1], v[startindex+2]);  // => (a, c, b, d, e, f)
	swap(v[startindex+2], v[startindex+4]);  // => (a, c, e, d, b, f)
	swap(v[startindex+3], v[startindex+4]);  // => (a, c, e, b, d, f)
	matrix.set(v, startindex);
}


/** Starts the definition of a new fill pattern. This operator
 *  expects 9 parameters for tiling patterns (see PS reference 4.9.2):
 *  @param[in] p the 9 values defining a tiling pattern (see PS reference 4.9.2):
 *  0: pattern type (0:none, 1:tiling, 2:shading)
 *  1: pattern ID
 *  2-5: lower left and upper right coordinates of pattern box
 *  6: horizontal distance of adjacent tiles
 *  7: vertical distance of adjacent tiles
 *  8: paint type (1: colored pattern, 2: uncolored pattern)
 *  9-14: pattern matrix */
void PsSpecialHandler::makepattern (vector<double> &p) {
	int pattern_type = static_cast<int>(p[0]);
	switch (pattern_type) {
		case 0:
			// pattern definition completed
			if (_savenode) {
				_xmlnode = _savenode;
				_savenode = nullptr;
			}
			break;
		case 1: {  // tiling pattern
			int id = static_cast<int>(p[1]);
			BoundingBox bbox(p[2], p[3], p[4], p[5]);
			const double &xstep=p[6], &ystep=p[7]; // horizontal and vertical distance of adjacent tiles
			int paint_type = static_cast<int>(p[8]);

			Matrix matrix;  // transformation matrix given together with pattern definition
			create_matrix(p, 9, matrix);
			matrix.lmultiply(_actions->getMatrix());

			unique_ptr<PSTilingPattern> pattern;
			if (paint_type == 1)
				pattern = util::make_unique<PSColoredTilingPattern>(id, bbox, matrix, xstep, ystep);
			else
				pattern = util::make_unique<PSUncoloredTilingPattern>(id, bbox, matrix, xstep, ystep);
			_savenode = _xmlnode;
			_xmlnode = pattern->getContainerNode();  // insert the following SVG elements into this node
			_patterns[id] = std::move(pattern);
			break;
		}
		case 2: {
			// define a shading pattern
		}
	}
}


/** Selects a previously defined fill pattern.
 *  0: pattern ID
 *  1-3: (optional) RGB values for uncolored tiling patterns
 *  further parameters depend on the pattern type */
void PsSpecialHandler::setpattern (vector<double> &p) {
	int patternID = static_cast<int>(p[0]);
	Color color;
	if (p.size() == 4)
		color.setRGB(p[1], p[2], p[3]);
	auto it = _patterns.find(patternID);
	if (it == _patterns.end())
		_pattern = nullptr;
	else {
		if (auto pattern = dynamic_cast<PSUncoloredTilingPattern*>(it->second.get()))
			pattern->setColor(color);
		it->second->apply(*_actions);
		if (auto pattern = dynamic_cast<PSTilingPattern*>(it->second.get()))
			_pattern = pattern;
		else
			_pattern = nullptr;
	}
}


/** Clears the current clipping path.
 *  @param[in] p not used */
void PsSpecialHandler::initclip (vector<double> &) {
	_clipStack.pushEmptyPath();
}


/** Assigns the current clipping path to the graphics path. */
void PsSpecialHandler::clippath (std::vector<double>&) {
	if (!_clipStack.empty())
		_clipStack.setPrependedPath();
}


/** Assigns a new clipping path to the graphics state using the current path.
 *  If the graphics state already contains a clipping path, the new one is
 *  computed by intersecting the current clipping path with the current graphics
 *  path (see PS language reference, 3rd edition, pp. 193, 542)
 *  @param[in] p not used
 *  @param[in] evenodd true: use even-odd fill algorithm, false: use nonzero fill algorithm */
void PsSpecialHandler::clip (vector<double>&, bool evenodd) {
	clip(_path, evenodd);
}


/** Assigns a new clipping path to the graphics state using the current path.
 *  If the graphics state already contains a clipping path, the new one is
 *  computed by intersecting the current one with the given path.
 *  @param[in] path path used to restrict the clipping region
 *  @param[in] evenodd true: use even-odd fill algorithm, false: use nonzero fill algorithm */
void PsSpecialHandler::clip (Path path, bool evenodd) {
	// when this method is called, _path contains the clipping path
	if (path.empty() || !_actions)
		return;

	Path::WindingRule windingRule = evenodd ? Path::WindingRule::EVEN_ODD : Path::WindingRule::NON_ZERO;
	path.setWindingRule(windingRule);

	if (!_actions->getMatrix().isIdentity())
		path.transform(_actions->getMatrix());
	if (_clipStack.prependedPath())
		path = PathClipper().unite(*_clipStack.prependedPath(), path);

	int oldID = _clipStack.topID();

	ostringstream oss;
	bool pathReplaced;
	if (!COMPUTE_CLIPPATHS_INTERSECTIONS || oldID < 1) {
		pathReplaced = _clipStack.replace(path);
		path.writeSVG(oss, SVGTree::RELATIVE_PATH_CMDS);
	}
	else {
		// compute the intersection of the current clipping path with the current graphics path
		const Path *oldPath = _clipStack.path();
		Path intersectedPath = PathClipper().intersect(*oldPath, path);
		pathReplaced = _clipStack.replace(intersectedPath);
		intersectedPath.writeSVG(oss, SVGTree::RELATIVE_PATH_CMDS);
	}
	if (pathReplaced) {
		auto pathElem = util::make_unique<XMLElement>("path");
		pathElem->addAttribute("d", oss.str());
		if (evenodd)
			pathElem->addAttribute("clip-rule", "evenodd");

		int newID = _clipStack.topID();
		auto clipElem = util::make_unique<XMLElement>("clipPath");
		clipElem->addAttribute("id", XMLString("clip")+XMLString(newID));
		if (!COMPUTE_CLIPPATHS_INTERSECTIONS && oldID)
			clipElem->addAttribute("clip-path", XMLString("url(#clip")+XMLString(oldID)+")");

		clipElem->append(std::move(pathElem));
		_actions->svgTree().appendToDefs(std::move(clipElem));
	}
}


/** Applies a gradient fill to the current graphics path. Vector p contains the shading parameters
 *  in the following order:
 *  - shading type (6=Coons, 7=tensor product)
 *  - color space (1=gray, 3=rgb, 4=cmyk)
 *  - 1.0 followed by the background color components based on the declared color space, or 0.0
 *  - 1.0 followed by the bounding box coordinates, or 0.0
 *  - geometry and color parameters depending on the shading type */
void PsSpecialHandler::shfill (vector<double> &params) {
	if (params.size() < 9)
		return;

	// collect common data relevant for all shading types
	int shadingTypeID = static_cast<int>(params[0]);
	ColorSpace colorSpace = Color::ColorSpace::RGB;
	switch (static_cast<int>(params[1])) {
		case 1: colorSpace = Color::ColorSpace::GRAY; break;
		case 3: colorSpace = Color::ColorSpace::RGB; break;
		case 4: colorSpace = Color::ColorSpace::CMYK; break;
	}
	VectorIterator<double> it(params);
	it += 2;     // skip shading type and color space
	// Get color to fill the whole mesh area before drawing the gradient colors on top of that background.
	// This is an optional parameter to shfill.
	bool bgcolorGiven = static_cast<bool>(*it++);
	Color bgcolor;
	if (bgcolorGiven)
		bgcolor.set(colorSpace, it);
	// Get clipping rectangle to limit the drawing area of the gradient mesh.
	// This is an optional parameter to shfill too.
	bool bboxGiven = static_cast<bool>(*it++);
	if (bboxGiven) { // bounding box given
		Path bboxPath;
		const double &x1 = *it++;
		const double &y1 = *it++;
		const double &x2 = *it++;
		const double &y2 = *it++;
		bboxPath.moveto(x1, y1);
		bboxPath.lineto(x2, y1);
		bboxPath.lineto(x2, y2);
		bboxPath.lineto(x1, y2);
		bboxPath.closepath();
		clip(bboxPath, false);
	}
	try {
		if (shadingTypeID == 5)
			processLatticeTriangularPatchMesh(colorSpace, it);
		else
			processSequentialPatchMesh(shadingTypeID, colorSpace, it);
	}
	catch (ShadingException &e) {
		Message::estream(false) << "PostScript error: " << e.what() << '\n';
		it.invalidate();  // stop processing the remaining patch data
	}
	catch (IteratorException &e) {
		Message::estream(false) << "PostScript error: incomplete shading data\n";
	}
	if (bboxGiven)
		_clipStack.pop();
}


/** Reads position and color data of a single shading patch from the data vector.
 *  @param[in] shadingTypeID PS shading type ID identifying the format of the subsequent patch data
 *  @param[in] edgeflag edge flag specifying how to connect the current patch to the preceding one
 *  @param[in] cspace color space used to compute the color gradient
 *  @param[in,out] it iterator used to sequentially access the patch data
 *  @param[out] points the points defining the geometry of the patch
 *  @param[out] colors the colors assigned to the vertices of the patch */
static void read_patch_data (ShadingPatch &patch, int edgeflag,
		VectorIterator<double> &it, vector<DPair> &points, vector<Color> &colors)
{
	// number of control points and colors required to define a single patch
	int numPoints = patch.numPoints(edgeflag);
	int numColors = patch.numColors(edgeflag);
	points.resize(numPoints);
	colors.resize(numColors);
	if (patch.psShadingType() == 4) {
		// format of a free-form triangular patch definition, where eN denotes
		// the edge of the corresponding vertex:
		// edge flag = 0, x1, y1, {color1}, e2, x2, y2, {color2}, e3, x3, y3, {color3}
		// edge flag > 0, x1, y1, {color1}
		for (int i=0; i < numPoints; i++) {
			if (i > 0) ++it;  // skip redundant edge flag from free-form triangular patch
			double x = *it++;
			double y = *it++;
			points[i] = DPair(x, y);
			colors[i].set(patch.colorSpace(), it);
		}
	}
	else if (patch.psShadingType() == 6 || patch.psShadingType() == 7) {
		// format of each Coons/tensor product patch definition:
		// edge flag = 0, x1, y1, ... , xn, yn, {color1}, {color2}, {color3}, {color4}
		// edge flag > 0, x5, y5, ... , xn, yn, {color3}, {color4}
		for (int i=0; i < numPoints; i++) {
			double x = *it++;
			double y = *it++;
			points[i] = DPair(x, y);
		}
		for (int i=0; i < numColors; i++)
			colors[i].set(patch.colorSpace(), it);
	}
}


class ShadingCallback : public ShadingPatch::Callback {
	public:
		ShadingCallback (SpecialActions &actions, XMLElement *parent, int clippathID)
			: _actions(actions)
		{
			auto group = util::make_unique<XMLElement>("g");
			_group = group.get();
			if (parent)
				parent->append(std::move(group));
			else
				actions.svgTree().appendToPage(std::move(group));
			if (clippathID > 0)
				_group->addAttribute("clip-path", XMLString("url(#clip")+XMLString(clippathID)+")");
		}

		void patchSegment (GraphicsPath<double> &path, const Color &color) override {
			if (!_actions.getMatrix().isIdentity())
				path.transform(_actions.getMatrix());

			// draw a single patch segment
			ostringstream oss;
			path.writeSVG(oss, SVGTree::RELATIVE_PATH_CMDS);
			auto pathElem = util::make_unique<XMLElement>("path");
			pathElem->addAttribute("d", oss.str());
			pathElem->addAttribute("fill", color.svgColorString());
			_group->append(std::move(pathElem));
		}

	private:
		SpecialActions &_actions;
		XMLElement *_group;
};


/** Handle all patch meshes whose patches and their connections can be processed sequentially.
 *  This comprises free-form triangular, Coons, and tensor-product patch meshes. */
void PsSpecialHandler::processSequentialPatchMesh (int shadingTypeID, ColorSpace colorSpace, VectorIterator<double> &it) {
	unique_ptr<ShadingPatch> previousPatch;
	while (it.valid()) {
		int edgeflag = static_cast<int>(*it++);
		vector<DPair> points;
		vector<Color> colors;
		unique_ptr<ShadingPatch> patch = ShadingPatch::create(shadingTypeID, colorSpace);
		read_patch_data(*patch, edgeflag, it, points, colors);
		patch->setPoints(points, edgeflag, previousPatch.get());
		patch->setColors(colors, edgeflag, previousPatch.get());
		ShadingCallback callback(*_actions, _xmlnode, _clipStack.topID());
#if 0
		if (bgcolorGiven) {
			// fill whole patch area with given background color
			GraphicsPath<double> outline = patch->getBoundaryPath();
			callback.patchSegment(outline, bgcolor);
		}
#endif
		patch->approximate(SHADING_SEGMENT_SIZE, SHADING_SEGMENT_OVERLAP, SHADING_SIMPLIFY_DELTA, callback);
		if (!_xmlnode) {
			// update bounding box
			BoundingBox bbox = patch->getBBox();
			bbox.transform(_actions->getMatrix());
			_actions->embed(bbox);
		}
		previousPatch = std::move(patch);
	}
}


void PsSpecialHandler::processLatticeTriangularPatchMesh (ColorSpace colorSpace, VectorIterator<double> &it) {
	int verticesPerRow = static_cast<int>(*it++);
	if (verticesPerRow < 2)
		return;

	struct PatchVertex {
		DPair point;
		Color color;
	};

	// hold two adjacent rows of vertices and colors
	vector<PatchVertex> row1(verticesPerRow);
	vector<PatchVertex> row2(verticesPerRow);
	vector<PatchVertex> *rowptr1 = &row1;
	vector<PatchVertex> *rowptr2 = &row2;
	// read data of first row
	for (int i=0; i < verticesPerRow; i++) {
		PatchVertex &vertex = (*rowptr1)[i];
		vertex.point.x(*it++);
		vertex.point.y(*it++);
		vertex.color.set(colorSpace, it);
	}
	LatticeTriangularPatch patch(colorSpace);
	ShadingCallback callback(*_actions, _xmlnode, _clipStack.topID());
	while (it.valid()) {
		// read next row
		for (int i=0; i < verticesPerRow; i++) {
			PatchVertex &vertex = (*rowptr2)[i];
			vertex.point.x(*it++);
			vertex.point.y(*it++);
			vertex.color.set(colorSpace, it);
		}
		// create triangular patches for the vertices of the two rows
		for (int i=0; i < verticesPerRow-1; i++) {
			const PatchVertex &v1 = (*rowptr1)[i], &v2 = (*rowptr1)[i+1];
			const PatchVertex &v3 = (*rowptr2)[i], &v4 = (*rowptr2)[i+1];
			patch.setPoints(v1.point, v2.point, v3.point);
			patch.setColors(v1.color, v2.color, v3.color);
			patch.approximate(SHADING_SEGMENT_SIZE, SHADING_SEGMENT_OVERLAP, SHADING_SIMPLIFY_DELTA, callback);

			patch.setPoints(v2.point, v3.point, v4.point);
			patch.setColors(v2.color, v3.color, v4.color);
			patch.approximate(SHADING_SEGMENT_SIZE, SHADING_SEGMENT_OVERLAP, SHADING_SIMPLIFY_DELTA, callback);
		}
		swap(rowptr1, rowptr2);
	}
}


/** Clears current path. */
void PsSpecialHandler::newpath (vector<double> &p) {
	bool calledByNewpathOp = (p[0] > 0);
	if (calledByNewpathOp)  // function triggered by PS operator 'newpath'?
		_clipStack.removePrependedPath();
	_path.clear();
}


void PsSpecialHandler::setmatrix (vector<double> &p) {
	if (_actions) {
		Matrix m;
		create_matrix(p, 0, m);
		_actions->setMatrix(m);
	}
}


// In contrast to SVG, PostScript transformations are applied in
// reverse order (M' = T*M). Thus, the transformation matrices must be
// left-multiplied in the following methods scale(), translate() and rotate().


void PsSpecialHandler::scale (vector<double> &p) {
	if (_actions) {
		Matrix m = _actions->getMatrix();
		ScalingMatrix s(p[0], p[1]);
		m.rmultiply(s);
		_actions->setMatrix(m);
	}
}


void PsSpecialHandler::translate (vector<double> &p) {
	if (_actions) {
		Matrix m = _actions->getMatrix();
		TranslationMatrix t(p[0], p[1]);
		m.rmultiply(t);
		_actions->setMatrix(m);
	}
}


void PsSpecialHandler::rotate (vector<double> &p) {
	if (_actions) {
		Matrix m = _actions->getMatrix();
		RotationMatrix r(p[0]);
		m.rmultiply(r);
		_actions->setMatrix(m);
	}
}


void PsSpecialHandler::setgray (vector<double> &p) {
	if (!_patternEnabled)
		_pattern = nullptr;
	_currentcolor.setGray(p[0]);
	if (_actions)
		_actions->setColor(_currentcolor);
}


void PsSpecialHandler::setrgbcolor (vector<double> &p) {
	if (!_patternEnabled)
		_pattern= nullptr;
	_currentcolor.setRGB(p[0], p[1], p[2]);
	if (_actions)
		_actions->setColor(_currentcolor);
}


void PsSpecialHandler::setcmykcolor (vector<double> &p) {
	if (!_patternEnabled)
		_pattern = nullptr;
	_currentcolor.setCMYK(p[0], p[1], p[2], p[3]);
	if (_actions)
		_actions->setColor(_currentcolor);
}


void PsSpecialHandler::sethsbcolor (vector<double> &p) {
	if (!_patternEnabled)
		_pattern = nullptr;
	_currentcolor.setHSB(p[0], p[1], p[2]);
	if (_actions)
		_actions->setColor(_currentcolor);
}


/** Sets the dash parameters used for stroking.
 *  @param[in] p dash pattern array m1,...,mn plus trailing dash offset */
void PsSpecialHandler::setdash (vector<double> &p) {
	_dashpattern.clear();
	for (size_t i=0; i < p.size()-1; i++)
		_dashpattern.push_back(scale(p[i]));
	_dashoffset = scale(p.back());
}


/** This method is called by the PSInterpreter if an PS operator has been executed. */
void PsSpecialHandler::executed () {
	if (_actions)
		_actions->progress("ps");
}


/** This method is called by PSInterpreter if the status of the output devices has changed.
 *  @param[in] p 1 if output device is the nulldevice, 1 otherwise */
void PsSpecialHandler::setnulldevice (vector<double> &p) {
	if (_actions) {
		if (p[0] != 0)
			_actions->lockOutput();   // prevent further SVG output
		else
			_actions->unlockOutput(); // enable SVG output again
	}
}

////////////////////////////////////////////

void PsSpecialHandler::ClippingStack::pushEmptyPath () {
	if (!_stack.empty())
		_stack.emplace(Entry());
}


void PsSpecialHandler::ClippingStack::push (const Path &path, int saveID) {
	shared_ptr<Path> prependedPath;
	if (!_stack.empty())
		prependedPath = _stack.top().prependedPath;
	if (path.empty())
		_stack.emplace(Entry(saveID));
	else
		_stack.emplace(Entry(path, ++_maxID, saveID));
	_stack.top().prependedPath = prependedPath;
}


/** Pops a single or several elements from the clipping stack.
 *  The method distinguishes between the following cases:
 *  1) saveID < 0 and grestoreall == false:
 *     pop top element if it was pushed by gsave (its saveID is < 0 as well)
 *  2) saveID < 0 and grestoreall == true
 *     repeat popping until stack is empty or the top element was pushed
 *     by save (its saveID is >= 0)
 *  3) saveID >= 0:
 *     pop all elements until the saveID of the top element equals parameter saveID */
void PsSpecialHandler::ClippingStack::pop (int saveID, bool grestoreall) {
	if (_stack.empty())
		return;

	if (saveID < 0) {                // grestore?
		if (_stack.top().saveID < 0)  // pushed by 'gsave'?
			_stack.pop();
		// pop all further elements pushed by 'gsave' if grestoreall == true
		while (grestoreall && !_stack.empty() && _stack.top().saveID < 0)
			_stack.pop();
	}
	else {
		// pop elements pushed by 'gsave'
		while (!_stack.empty() && _stack.top().saveID != saveID)
			_stack.pop();
		// pop element pushed by 'save'
		if (!_stack.empty())
			_stack.pop();
	}
}


/** Returns a pointer to the path on top of the stack, or 0 if the stack is empty. */
const PsSpecialHandler::Path* PsSpecialHandler::ClippingStack::path () const {
	return _stack.empty() ? nullptr : _stack.top().path.get();
}


/** Returns a pointer to the path on top of the stack, or 0 if the stack is empty. */
const PsSpecialHandler::Path* PsSpecialHandler::ClippingStack::prependedPath () const {
	return _stack.empty() ? nullptr : _stack.top().prependedPath.get();
}


void PsSpecialHandler::ClippingStack::removePrependedPath () {
	if (!_stack.empty())
		_stack.top().prependedPath = nullptr;
}


/** Pops all elements from the stack. */
void PsSpecialHandler::ClippingStack::clear() {
	while (!_stack.empty())
		_stack.pop();
}


/** Replaces the top path by a new one.
 *  @param[in] path new path to put on the stack
 *  @return true if the new path differs from the previous one */
bool PsSpecialHandler::ClippingStack::replace (const Path &path) {
	if (_stack.empty())
		push(path, -1);
	else if (_stack.top().path && path == *_stack.top().path)
		return false;
	else {
		_stack.top().path = make_shared<Path>(path);
		_stack.top().pathID = ++_maxID;
	}
	return true;
}


/** Duplicates the top element, i.e. the top element is pushed again. */
void PsSpecialHandler::ClippingStack::dup (int saveID) {
	_stack.emplace(_stack.empty() ? Entry() : _stack.top());
	_stack.top().saveID = saveID;
}


void PsSpecialHandler::ClippingStack::setPrependedPath () {
	if (!_stack.empty())
		_stack.top().prependedPath = _stack.top().path;
}


vector<const char*> PsSpecialHandler::prefixes() const {
	vector<const char*> pfx {
		"header=",    // read and execute PS header file prior to the following PS statements
		"pdffile=",   // process PDF file
		"psfile=",    // read and execute PS file
		"PSfile=",    // dito
		"ps:",        // execute literal PS code wrapped by @beginspecial and @endspecial
		"ps::",       // execute literal PS code without additional adaption of the drawing position
		"!",          // execute literal PS header code following this prefix
		"\"",         // execute literal PS code following this prefix
		"pst:",       // dito
		"PST:",       // same as "ps:"
	};
	return pfx;
}
