/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  gatecat <gatecat@ds0.me>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <algorithm>
#include <boost/optional.hpp>
#include <iterator>
#include <queue>
#include "cells.h"
#include "chain_utils.h"
#include "design_utils.h"
#include "globals.h"
#include "log.h"
#include "timing.h"
#include "util.h"
NEXTPNR_NAMESPACE_BEGIN

static bool is_nextpnr_iob(Context *ctx, CellInfo *cell)
{
    return cell->type == ctx->id("$nextpnr_ibuf") || cell->type == ctx->id("$nextpnr_obuf") ||
           cell->type == ctx->id("$nextpnr_iobuf");
}

class Ecp5Packer
{
  public:
    Ecp5Packer(Context *ctx) : ctx(ctx){};

  private:
    // Process the contents of packed_cells and new_cells
    void flush_cells()
    {
        for (auto pcell : packed_cells) {
            ctx->cells.erase(pcell);
        }
        for (auto &ncell : new_cells) {
            ctx->cells[ncell->name] = std::move(ncell);
        }
        packed_cells.clear();
        new_cells.clear();
    }

    // Print logic usage
    int available_slices = 0;
    void print_logic_usage()
    {
        int total_luts = 0, total_ffs = 0;
        int total_ramluts = 0, total_ramwluts = 0;
        for (auto bel : ctx->getBels()) {
            if (ctx->getBelType(bel) == id_TRELLIS_SLICE) {
                available_slices += 1;
                total_luts += 2;
                total_ffs += 2;
                Loc l = ctx->getBelLocation(bel);
                if (l.z == 0 || l.z == 1)
                    total_ramluts += 2;
                if (l.z == 2)
                    total_ramwluts += 2;
            }
        }
        int used_lgluts = 0, used_cyluts = 0, used_ramluts = 0, used_ramwluts = 0, used_ffs = 0;
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (is_lut(ctx, ci))
                ++used_lgluts;
            if (is_carry(ctx, ci))
                used_cyluts += 2;
            if (is_dpram(ctx, ci)) {
                used_ramluts += 4;
                used_ramwluts += 2;
            }
            if (is_ff(ctx, ci))
                used_ffs += 2;
        }
        log_info("Logic utilisation before packing:\n");
        auto pc = [](int used, int total) { return 100 * used / total; };
        int used_luts = used_lgluts + used_cyluts + used_ramluts + used_ramwluts;
        log_info("    Total LUT4s:     %5d/%5d %5d%%\n", used_luts, total_luts, pc(used_luts, total_luts));
        log_info("        logic LUTs:  %5d/%5d %5d%%\n", used_lgluts, total_luts, pc(used_lgluts, total_luts));
        log_info("        carry LUTs:  %5d/%5d %5d%%\n", used_cyluts, total_luts, pc(used_cyluts, total_luts));
        log_info("          RAM LUTs:  %5d/%5d %5d%%\n", used_ramluts, total_ramluts, pc(used_ramluts, total_ramluts));
        log_info("         RAMW LUTs:  %5d/%5d %5d%%\n", used_ramwluts, total_ramwluts,
                 pc(used_ramwluts, total_ramwluts));
        log_break();
        log_info("     Total DFFs:     %5d/%5d %5d%%\n", used_ffs, total_ffs, pc(used_ffs, total_ffs));
        log_break();
    }

    // Find FFs associated with LUTs, or LUT expansion muxes
    void find_lutff_pairs()
    {
        log_info("Finding LUTFF pairs...\n");
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (is_lut(ctx, ci) || is_pfumx(ctx, ci) || is_l6mux(ctx, ci)) {
                NetInfo *znet = ci->ports.at(id_Z).net;
                if (znet != nullptr) {
                    CellInfo *ff = net_only_drives(ctx, znet, is_ff, id_DI, false);
                    // Can't combine preload FF with LUT due to conflict on M
                    if (ff != nullptr && ff->getPort(id_M) == nullptr) {
                        lutffPairs[ci->name] = ff->name;
                        fflutPairs[ff->name] = ci->name;
                    }
                }
            }
        }
    }

    // Check if a flipflop is available in a slice
    bool is_ff_available(CellInfo *slice, int ff)
    {
        if (slice->getPort((ff == 1) ? id_Q1 : id_Q0) != nullptr)
            return false;
        if (slice->getPort((ff == 1) ? id_M1 : id_M0) != nullptr)
            return false;
        return true;
    }

    // Check if a flipflop can be added to a slice
    bool can_add_ff_to_slice(CellInfo *slice, CellInfo *ff)
    {
        std::string clkmux = str_or_default(ff->params, id_CLKMUX, "CLK");
        std::string lsrmux = str_or_default(ff->params, id_LSRMUX, "LSR");

        bool has_dpram = str_or_default(slice->params, id_MODE, "LOGIC") == "DPRAM";
        if (has_dpram) {
            std::string wckmux = str_or_default(slice->params, id_WCKMUX, "WCK");
            std::string wremux = str_or_default(slice->params, id_WREMUX, "WRE");
            if (wckmux != clkmux && !(wckmux == "WCK" && clkmux == "CLK"))
                return false;
            if (wremux != lsrmux && !(wremux == "WRE" && lsrmux == "LSR"))
                return false;
        }
        bool has_ff0 = slice->getPort(id_Q0) != nullptr;
        bool has_ff1 = slice->getPort(id_Q1) != nullptr;
        if (!has_ff0 && !has_ff1)
            return true;
        if (str_or_default(ff->params, id_GSR, "DISABLED") != str_or_default(slice->params, id_GSR, "DISABLED"))
            return false;
        if (str_or_default(ff->params, id_SRMODE, "LSR_OVER_CE") !=
            str_or_default(slice->params, id_SRMODE, "LSR_OVER_CE"))
            return false;
        if (str_or_default(ff->params, id_CEMUX, "1") != str_or_default(slice->params, id_CEMUX, "1"))
            return false;
        if (str_or_default(ff->params, id_LSRMUX, "LSR") != str_or_default(slice->params, id_LSRMUX, "LSR"))
            return false;
        if (str_or_default(ff->params, id_CLKMUX, "CLK") != str_or_default(slice->params, id_CLKMUX, "CLK"))
            return false;
        if (net_or_nullptr(ff, id_CLK) != net_or_nullptr(slice, id_CLK))
            return false;
        if (net_or_nullptr(ff, id_CE) != net_or_nullptr(slice, id_CE))
            return false;
        if (net_or_nullptr(ff, id_LSR) != net_or_nullptr(slice, id_LSR))
            return false;
        return true;
    }

    const NetInfo *net_or_nullptr(CellInfo *cell, IdString port)
    {
        auto fnd = cell->ports.find(port);
        if (fnd == cell->ports.end())
            return nullptr;
        else
            return fnd->second.net;
    }

    // Return whether two FFs can be packed together in the same slice
    bool can_pack_ffs(CellInfo *ff0, CellInfo *ff1)
    {
        if (str_or_default(ff0->params, id_GSR, "DISABLED") != str_or_default(ff1->params, id_GSR, "DISABLED"))
            return false;
        if (str_or_default(ff0->params, id_SRMODE, "LSR_OVER_CE") !=
            str_or_default(ff1->params, id_SRMODE, "LSR_OVER_CE"))
            return false;
        if (str_or_default(ff0->params, id_CEMUX, "1") != str_or_default(ff1->params, id_CEMUX, "1"))
            return false;
        if (str_or_default(ff0->params, id_LSRMUX, "LSR") != str_or_default(ff1->params, id_LSRMUX, "LSR"))
            return false;
        if (str_or_default(ff0->params, id_CLKMUX, "CLK") != str_or_default(ff1->params, id_CLKMUX, "CLK"))
            return false;
        if (net_or_nullptr(ff0, id_CLK) != net_or_nullptr(ff1, id_CLK))
            return false;
        if (net_or_nullptr(ff0, id_CE) != net_or_nullptr(ff1, id_CE))
            return false;
        if (net_or_nullptr(ff0, id_LSR) != net_or_nullptr(ff1, id_LSR))
            return false;
        return true;
    }

    // Return whether or not an FF can be added to a tile (pairing checks must also be done using the fn above)
    bool can_add_ff_to_tile(const std::vector<CellInfo *> &tile_ffs, CellInfo *ff0)
    {
        for (const auto &existing : tile_ffs) {
            if (net_or_nullptr(existing, id_CLK) != net_or_nullptr(ff0, id_CLK))
                return false;
            if (net_or_nullptr(existing, id_LSR) != net_or_nullptr(ff0, id_LSR))
                return false;
            if (str_or_default(existing->params, id_CLKMUX, "CLK") != str_or_default(ff0->params, id_CLKMUX, "CLK"))
                return false;
            if (str_or_default(existing->params, id_LSRMUX, "LSR") != str_or_default(ff0->params, id_LSRMUX, "LSR"))
                return false;
            if (str_or_default(existing->params, id_SRMODE, "LSR_OVER_CE") !=
                str_or_default(ff0->params, id_SRMODE, "LSR_OVER_CE"))
                return false;
        }
        return true;
    }

    // Return true if a FF can be added to a DPRAM slice
    bool can_pack_ff_dram(CellInfo *dpram, CellInfo *ff)
    {
        if (ff->getPort(id_M) != nullptr)
            return false; // skip PRLD FFs due to M/DI conflict
        std::string wckmux = str_or_default(dpram->params, id_WCKMUX, "WCK");
        std::string clkmux = str_or_default(ff->params, id_CLKMUX, "CLK");
        if (wckmux != clkmux && !(wckmux == "WCK" && clkmux == "CLK"))
            return false;
        std::string wremux = str_or_default(dpram->params, id_WREMUX, "WRE");
        std::string lsrmux = str_or_default(ff->params, id_LSRMUX, "LSR");
        if (wremux != lsrmux && !(wremux == "WRE" && lsrmux == "LSR"))
            return false;
        return true;
    }

    // Return true if two LUTs can be paired considering FF compatibility
    bool can_pack_lutff(IdString lut0, IdString lut1)
    {
        auto ff0 = lutffPairs.find(lut0), ff1 = lutffPairs.find(lut1);
        if (ff0 != lutffPairs.end() && ff1 != lutffPairs.end()) {
            return can_pack_ffs(ctx->cells.at(ff0->second).get(), ctx->cells.at(ff1->second).get());
        } else {
            return true;
        }
    }

    // Find "closely connected" LUTs and pair them together
    void pair_luts()
    {
        log_info("Finding LUT-LUT pairs...\n");
        pool<IdString> procdLuts;
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (is_lut(ctx, ci) && procdLuts.find(cell.first) == procdLuts.end()) {
                NetInfo *znet = ci->ports.at(id_Z).net;
                std::vector<NetInfo *> inpnets;
                if (znet != nullptr) {
                    for (auto user : znet->users) {
                        if (is_lut(ctx, user.cell) && user.cell != ci &&
                            procdLuts.find(user.cell->name) == procdLuts.end()) {
                            if (can_pack_lutff(ci->name, user.cell->name)) {
                                procdLuts.insert(ci->name);
                                procdLuts.insert(user.cell->name);
                                lutPairs[ci->name] = user.cell->name;
                                goto paired;
                            }
                        }
                    }
                    if (false) {
                    paired:
                        continue;
                    }
                }
                if (lutffPairs.find(ci->name) != lutffPairs.end()) {
                    NetInfo *qnet = ctx->cells.at(lutffPairs[ci->name])->ports.at(id_Q).net;
                    if (qnet != nullptr) {
                        for (auto user : qnet->users) {
                            if (is_lut(ctx, user.cell) && user.cell != ci &&
                                procdLuts.find(user.cell->name) == procdLuts.end()) {
                                if (can_pack_lutff(ci->name, user.cell->name)) {
                                    procdLuts.insert(ci->name);
                                    procdLuts.insert(user.cell->name);
                                    lutPairs[ci->name] = user.cell->name;
                                    goto paired_ff;
                                }
                            }
                        }
                        if (false) {
                        paired_ff:
                            continue;
                        }
                    }
                }
                for (const char *inp : {"A", "B", "C", "D"}) {
                    if (!ci->ports.count(ctx->id(inp)))
                        continue;
                    NetInfo *innet = ci->ports.at(ctx->id(inp)).net;
                    if (innet != nullptr && innet->driver.cell != nullptr) {
                        CellInfo *drv = innet->driver.cell;
                        if (is_lut(ctx, drv) && drv != ci && innet->driver.port == id_Z) {
                            if (procdLuts.find(drv->name) == procdLuts.end()) {
                                if (can_pack_lutff(ci->name, drv->name)) {
                                    procdLuts.insert(ci->name);
                                    procdLuts.insert(drv->name);
                                    lutPairs[ci->name] = drv->name;
                                    goto paired_inlut;
                                }
                            }
                        } else if (is_ff(ctx, drv) && innet->driver.port == id_Q) {
                            auto fflut = fflutPairs.find(drv->name);
                            if (fflut != fflutPairs.end() && fflut->second != ci->name &&
                                procdLuts.find(fflut->second) == procdLuts.end()) {
                                if (can_pack_lutff(ci->name, fflut->second)) {
                                    procdLuts.insert(ci->name);
                                    procdLuts.insert(fflut->second);
                                    lutPairs[ci->name] = fflut->second;
                                    goto paired_inlut;
                                }
                            }
                        }
                    }
                }

                // Pack LUTs feeding the same CCU2, RAM or DFF into a SLICE
                if (znet != nullptr && znet->users.entries() < 10) {
                    for (auto user : znet->users) {
                        if (is_lc(ctx, user.cell) || user.cell->type == id_DP16KD || is_ff(ctx, user.cell)) {
                            for (auto port : user.cell->ports) {
                                if (port.second.type != PORT_IN || port.second.net == nullptr ||
                                    port.second.net == znet)
                                    continue;
                                if (port.second.net->users.entries() > 10)
                                    continue;
                                CellInfo *drv = port.second.net->driver.cell;
                                if (drv == nullptr)
                                    continue;
                                if (is_lut(ctx, drv) && !procdLuts.count(drv->name) &&
                                    can_pack_lutff(ci->name, drv->name)) {
                                    procdLuts.insert(ci->name);
                                    procdLuts.insert(drv->name);
                                    lutPairs[ci->name] = drv->name;
                                    goto paired_inlut;
                                }
                            }
                        }
                    }
                }

                // Pack LUTs sharing an input with a simple fanout-based heuristic
                for (const char *inp : {"A", "B", "C", "D"}) {
                    if (!ci->ports.count(ctx->id(inp)))
                        continue;
                    NetInfo *innet = ci->ports.at(ctx->id(inp)).net;
                    if (innet != nullptr && innet->users.entries() < 5 && innet->users.entries() > 1)
                        inpnets.push_back(innet);
                }
                std::sort(inpnets.begin(), inpnets.end(),
                          [&](const NetInfo *a, const NetInfo *b) { return a->users.entries() < b->users.entries(); });
                for (auto inet : inpnets) {
                    for (auto &user : inet->users) {
                        if (user.cell == nullptr || user.cell == ci || !is_lut(ctx, user.cell))
                            continue;
                        if (procdLuts.count(user.cell->name))
                            continue;
                        if (can_pack_lutff(ci->name, user.cell->name)) {
                            procdLuts.insert(ci->name);
                            procdLuts.insert(user.cell->name);
                            lutPairs[ci->name] = user.cell->name;
                            goto paired_inlut;
                        }
                    }
                }

                if (false) {
                paired_inlut:
                    continue;
                }
            }
        }
        if (ctx->debug) {
            log_info("Singleton LUTs (packer QoR debug): \n");
            for (auto &cell : ctx->cells)
                if (is_lut(ctx, cell.second.get()) && !procdLuts.count(cell.first))
                    log_info("     %s\n", cell.first.c_str(ctx));
        }
    }

    // Return true if an port is a top level port that provides its own IOBUF
    bool is_top_port(PortRef &port)
    {
        if (port.cell == nullptr)
            return false;
        if (port.cell->type == id_DCUA) {
            return port.port == id_CH0_HDINP || port.port == id_CH0_HDINN || port.port == id_CH0_HDOUTP ||
                   port.port == id_CH0_HDOUTN || port.port == id_CH1_HDINP || port.port == id_CH1_HDINN ||
                   port.port == id_CH1_HDOUTP || port.port == id_CH1_HDOUTN;
        } else if (port.cell->type == id_EXTREFB) {
            return port.port == id_REFCLKP || port.port == id_REFCLKN;
        } else {
            return false;
        }
    }

    // Return true if a net only drives a top port
    bool drives_top_port(NetInfo *net, PortRef &tp)
    {
        if (net == nullptr)
            return false;
        for (auto user : net->users) {
            if (is_top_port(user)) {
                if (net->users.entries() > 1)
                    log_error("   port %s.%s must be connected to (and only to) a top level pin\n",
                              user.cell->name.c_str(ctx), user.port.c_str(ctx));
                tp = user;
                return true;
            }
        }
        if (net->driver.cell != nullptr && is_top_port(net->driver)) {
            if (net->users.entries() > 1)
                log_error("   port %s.%s must be connected to (and only to) a top level pin\n",
                          net->driver.cell->name.c_str(ctx), net->driver.port.c_str(ctx));
            tp = net->driver;
            return true;
        }
        return false;
    }

    // Simple "packer" to remove nextpnr IOBUFs, this assumes IOBUFs are manually instantiated
    void pack_io()
    {
        log_info("Packing IOs..\n");

        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (is_nextpnr_iob(ctx, ci)) {
                CellInfo *trio = nullptr;
                NetInfo *ionet = nullptr;
                PortRef tp;
                if (ci->type == ctx->id("$nextpnr_ibuf") || ci->type == ctx->id("$nextpnr_iobuf")) {
                    ionet = ci->ports.at(id_O).net;
                    trio = net_only_drives(ctx, ionet, is_trellis_io, id_B, true, ci);

                } else if (ci->type == ctx->id("$nextpnr_obuf")) {
                    ionet = ci->ports.at(id_I).net;
                    trio = net_only_drives(ctx, ci->ports.at(id_I).net, is_trellis_io, id_B, true, ci);
                }
                if (bool_or_default(ctx->settings, ctx->id("arch.ooc"))) {
                    // No IO buffer insertion in out-of-context mode, just remove the nextpnr buffer
                    // and leave the top level port
                    for (auto &port : ci->ports)
                        ci->disconnectPort(port.first);
                } else if (trio != nullptr) {
                    // Trivial case, TRELLIS_IO used. Just remove the IOBUF
                    log_info("%s feeds TRELLIS_IO %s, removing %s %s.\n", ci->name.c_str(ctx), trio->name.c_str(ctx),
                             ci->type.c_str(ctx), ci->name.c_str(ctx));

                    NetInfo *net = trio->ports.at(id_B).net;
                    if (((ci->type == ctx->id("$nextpnr_ibuf") || ci->type == ctx->id("$nextpnr_iobuf")) &&
                         net->users.entries() > 1) ||
                        (ci->type == ctx->id("$nextpnr_obuf") &&
                         (net->users.entries() > 2 || net->driver.cell != nullptr)) ||
                        (ci->type == ctx->id("$nextpnr_iobuf") && ci->ports.at(id_I).net != nullptr &&
                         ci->ports.at(id_I).net->driver.cell != nullptr))
                        log_error("Pin B of %s '%s' connected to more than a single top level IO.\n",
                                  trio->type.c_str(ctx), trio->name.c_str(ctx));
                    if (net != nullptr) {
                        if (net->clkconstr != nullptr && trio->ports.count(id_O)) {
                            NetInfo *onet = trio->ports.at(id_O).net;
                            if (onet != nullptr && !onet->clkconstr) {
                                // Move clock constraint from IO pad to input buffer output
                                std::swap(net->clkconstr, onet->clkconstr);
                            }
                        }
                    }
                } else if (drives_top_port(ionet, tp)) {
                    log_info("%s feeds %s %s.%s, removing %s %s.\n", ci->name.c_str(ctx), tp.cell->type.c_str(ctx),
                             tp.cell->name.c_str(ctx), tp.port.c_str(ctx), ci->type.c_str(ctx), ci->name.c_str(ctx));
                    if (ionet != nullptr) {
                        ctx->nets.erase(ionet->name);
                        tp.cell->ports.at(tp.port).net = nullptr;
                    }
                    if (ci->type == ctx->id("$nextpnr_iobuf")) {
                        NetInfo *net2 = ci->ports.at(id_I).net;
                        if (net2 != nullptr) {
                            ctx->nets.erase(net2->name);
                        }
                    }
                } else {
                    // Create a TRELLIS_IO buffer
                    std::unique_ptr<CellInfo> tr_cell =
                            create_ecp5_cell(ctx, id_TRELLIS_IO, ci->name.str(ctx) + "$tr_io");
                    nxio_to_tr(ctx, ci, tr_cell.get(), new_cells, packed_cells);
                    new_cells.push_back(std::move(tr_cell));
                    trio = new_cells.back().get();
                }
                for (auto port : ci->ports)
                    ci->disconnectPort(port.first);
                packed_cells.insert(ci->name);
                if (trio != nullptr) {
                    for (const auto &attr : ci->attrs)
                        trio->attrs[attr.first] = attr.second;

                    auto loc_attr = trio->attrs.find(id_LOC);
                    if (loc_attr != trio->attrs.end()) {
                        std::string pin = loc_attr->second.as_string();
                        BelId pinBel = ctx->get_package_pin_bel(pin);
                        if (pinBel == BelId()) {
                            log_error("IO pin '%s' constrained to pin '%s', which does not exist for package '%s'.\n",
                                      trio->name.c_str(ctx), pin.c_str(), ctx->args.package.c_str());
                        } else {
                            log_info("pin '%s' constrained to Bel '%s'.\n", trio->name.c_str(ctx),
                                     ctx->nameOfBel(pinBel));
                        }
                        trio->attrs[id_BEL] = ctx->getBelName(pinBel).str(ctx);
                    }
                }
            }
        }
        flush_cells();
    }

    // Pass to pack LUT5s into a newly created slice
    void pack_lut5xs()
    {
        log_info("Packing LUT5-7s...\n");
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (is_pfumx(ctx, ci)) {
                std::unique_ptr<CellInfo> packed =
                        create_ecp5_cell(ctx, id_TRELLIS_SLICE, ci->name.str(ctx) + "_SLICE");
                NetInfo *f0 = ci->ports.at(id_BLUT).net;
                if (f0 == nullptr)
                    log_error("PFUMX '%s' has disconnected port 'BLUT'\n", ci->name.c_str(ctx));
                NetInfo *f1 = ci->ports.at(id_ALUT).net;
                if (f1 == nullptr)
                    log_error("PFUMX '%s' has disconnected port 'ALUT'\n", ci->name.c_str(ctx));
                CellInfo *lut0 = net_driven_by(ctx, f0, is_lut, id_Z);
                CellInfo *lut1 = net_driven_by(ctx, f1, is_lut, id_Z);
                if (lut0 == nullptr)
                    log_error("PFUMX '%s' has BLUT driven by cell other than a LUT\n", ci->name.c_str(ctx));
                if (lut1 == nullptr)
                    log_error("PFUMX '%s' has ALUT driven by cell other than a LUT\n", ci->name.c_str(ctx));
                if (ctx->verbose)
                    log_info("   mux '%s' forms part of a LUT5\n", cell.first.c_str(ctx));
                lut0->movePortTo(id_A, packed.get(), id_A0);
                lut0->movePortTo(id_B, packed.get(), id_B0);
                lut0->movePortTo(id_C, packed.get(), id_C0);
                lut0->movePortTo(id_D, packed.get(), id_D0);
                lut1->movePortTo(id_A, packed.get(), id_A1);
                lut1->movePortTo(id_B, packed.get(), id_B1);
                lut1->movePortTo(id_C, packed.get(), id_C1);
                lut1->movePortTo(id_D, packed.get(), id_D1);
                ci->movePortTo(id_C0, packed.get(), id_M0);
                ci->movePortTo(id_Z, packed.get(), id_OFX0);
                packed->params[id_LUT0_INITVAL] = get_or_default(lut0->params, id_INIT, Property(0, 16));
                packed->params[id_LUT1_INITVAL] = get_or_default(lut1->params, id_INIT, Property(0, 16));

                ctx->nets.erase(f0->name);
                ctx->nets.erase(f1->name);
                sliceUsage[packed->name].lut0_used = true;
                sliceUsage[packed->name].lut1_used = true;
                sliceUsage[packed->name].mux5_used = true;

                if (lutffPairs.find(ci->name) != lutffPairs.end()) {
                    CellInfo *ff = ctx->cells.at(lutffPairs[ci->name]).get();
                    ff_to_slice(ctx, ff, packed.get(), 0, true);
                    packed_cells.insert(ff->name);
                    sliceUsage[packed->name].ff0_used = true;
                    lutffPairs.erase(ci->name);
                    fflutPairs.erase(ff->name);
                }

                new_cells.push_back(std::move(packed));
                packed_cells.insert(lut0->name);
                packed_cells.insert(lut1->name);
                packed_cells.insert(ci->name);
            }
        }
        flush_cells();
        // Pack LUT6s
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (is_l6mux(ctx, ci)) {
                NetInfo *ofx0_0 = ci->ports.at(id_D0).net;
                if (ofx0_0 == nullptr)
                    log_error("L6MUX21 '%s' has disconnected port 'D0'\n", ci->name.c_str(ctx));
                NetInfo *ofx0_1 = ci->ports.at(id_D1).net;
                if (ofx0_1 == nullptr)
                    log_error("L6MUX21 '%s' has disconnected port 'D1'\n", ci->name.c_str(ctx));
                CellInfo *slice0 = net_driven_by(ctx, ofx0_0, is_lc, id_OFX0);
                CellInfo *slice1 = net_driven_by(ctx, ofx0_1, is_lc, id_OFX0);
                if (slice0 == nullptr) {
                    if (!net_driven_by(ctx, ofx0_0, is_l6mux, id_Z) && !net_driven_by(ctx, ofx0_0, is_lc, id_OFX1))
                        log_error("L6MUX21 '%s' has D0 driven by cell other than a SLICE OFX0 but not a LUT7 mux "
                                  "('%s.%s')\n",
                                  ci->name.c_str(ctx), ofx0_0->driver.cell->name.c_str(ctx),
                                  ofx0_0->driver.port.c_str(ctx));
                    continue;
                }
                if (slice1 == nullptr) {
                    if (!net_driven_by(ctx, ofx0_1, is_l6mux, id_Z) && !net_driven_by(ctx, ofx0_1, is_lc, id_OFX1))
                        log_error("L6MUX21 '%s' has D1 driven by cell other than a SLICE OFX0 but not a LUT7 mux "
                                  "('%s.%s')\n",
                                  ci->name.c_str(ctx), ofx0_0->driver.cell->name.c_str(ctx),
                                  ofx0_0->driver.port.c_str(ctx));
                    continue;
                }
                if (ctx->verbose)
                    log_info("   mux '%s' forms part of a LUT6\n", cell.first.c_str(ctx));
                ci->movePortTo(id_D0, slice1, id_FXA);
                ci->movePortTo(id_D1, slice1, id_FXB);
                ci->movePortTo(id_SD, slice1, id_M1);
                ci->movePortTo(id_Z, slice1, id_OFX1);
                slice0->constr_z = 1;
                slice0->constr_x = 0;
                slice0->constr_y = 0;
                slice0->cluster = slice1->name;
                slice1->constr_z = 0;
                slice1->constr_abs_z = true;
                slice1->constr_children.push_back(slice0);
                slice1->cluster = slice1->name;

                if (lutffPairs.find(ci->name) != lutffPairs.end()) {
                    CellInfo *ff = ctx->cells.at(lutffPairs[ci->name]).get();
                    ff_to_slice(ctx, ff, slice1, 1, true);
                    packed_cells.insert(ff->name);
                    sliceUsage[slice1->name].ff1_used = true;
                    lutffPairs.erase(ci->name);
                    fflutPairs.erase(ff->name);
                }

                packed_cells.insert(ci->name);
            }
        }
        flush_cells();
        // Pack LUT7s
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (is_l6mux(ctx, ci)) {
                NetInfo *ofx1_0 = ci->ports.at(id_D0).net;
                if (ofx1_0 == nullptr)
                    log_error("L6MUX21 '%s' has disconnected port 'D0'\n", ci->name.c_str(ctx));
                NetInfo *ofx1_1 = ci->ports.at(id_D1).net;
                if (ofx1_1 == nullptr)
                    log_error("L6MUX21 '%s' has disconnected port 'D1'\n", ci->name.c_str(ctx));
                CellInfo *slice1 = net_driven_by(ctx, ofx1_0, is_lc, id_OFX1);
                CellInfo *slice3 = net_driven_by(ctx, ofx1_1, is_lc, id_OFX1);
                if (slice1 == nullptr)
                    log_error("L6MUX21 '%s' has D0 driven by cell other than a SLICE OFX ('%s.%s')\n",
                              ci->name.c_str(ctx), ofx1_0->driver.cell->name.c_str(ctx),
                              ofx1_0->driver.port.c_str(ctx));
                if (slice3 == nullptr)
                    log_error("L6MUX21 '%s' has D1 driven by cell other than a SLICE OFX ('%s.%s')\n",
                              ci->name.c_str(ctx), ofx1_1->driver.cell->name.c_str(ctx),
                              ofx1_1->driver.port.c_str(ctx));

                NetInfo *fxa_0 = slice1->ports.at(id_FXA).net;
                if (fxa_0 == nullptr)
                    log_error("SLICE '%s' has disconnected port 'FXA'\n", slice1->name.c_str(ctx));
                NetInfo *fxa_1 = slice3->ports.at(id_FXA).net;
                if (fxa_1 == nullptr)
                    log_error("SLICE '%s' has disconnected port 'FXA'\n", slice3->name.c_str(ctx));

                CellInfo *slice0 = net_driven_by(ctx, fxa_0, is_lc, id_OFX0);
                CellInfo *slice2 = net_driven_by(ctx, fxa_1, is_lc, id_OFX0);
                if (slice0 == nullptr)
                    log_error("SLICE '%s' has FXA driven by cell other than a SLICE OFX0 ('%s.%s')\n",
                              slice1->name.c_str(ctx), fxa_0->driver.cell->name.c_str(ctx),
                              fxa_0->driver.port.c_str(ctx));
                if (slice2 == nullptr)
                    log_error("SLICE '%s' has FXA driven by cell other than a SLICE OFX0 ('%s.%s')\n",
                              slice3->name.c_str(ctx), fxa_1->driver.cell->name.c_str(ctx),
                              fxa_1->driver.port.c_str(ctx));

                ci->movePortTo(id_D0, slice2, id_FXA);
                ci->movePortTo(id_D1, slice2, id_FXB);
                ci->movePortTo(id_SD, slice2, id_M1);
                ci->movePortTo(id_Z, slice2, id_OFX1);

                for (auto slice : {slice0, slice1, slice2, slice3}) {
                    slice->constr_children.clear();
                    slice->constr_abs_z = false;
                    slice->constr_x = 0;
                    slice->constr_y = 0;
                    slice->constr_z = 0;
                    slice->cluster = ClusterId();
                }
                slice3->constr_children.clear();
                slice3->constr_abs_z = true;
                slice3->constr_z = 0;
                slice3->cluster = slice3->name;

                slice2->constr_children.clear();
                slice2->constr_abs_z = true;
                slice2->constr_z = 1;
                slice2->constr_x = 0;
                slice2->constr_y = 0;
                slice2->cluster = slice3->name;
                slice3->constr_children.push_back(slice2);

                slice1->constr_children.clear();
                slice1->constr_abs_z = true;
                slice1->constr_z = 2;
                slice1->constr_x = 0;
                slice1->constr_y = 0;
                slice1->cluster = slice3->name;
                slice3->constr_children.push_back(slice1);

                slice0->constr_children.clear();
                slice0->constr_abs_z = true;
                slice0->constr_z = 3;
                slice0->constr_x = 0;
                slice0->constr_y = 0;
                slice0->cluster = slice3->name;
                slice3->constr_children.push_back(slice0);

                if (lutffPairs.find(ci->name) != lutffPairs.end()) {
                    CellInfo *ff = ctx->cells.at(lutffPairs[ci->name]).get();
                    ff_to_slice(ctx, ff, slice2, 1, true);
                    packed_cells.insert(ff->name);
                    sliceUsage[slice2->name].ff1_used = true;
                    lutffPairs.erase(ci->name);
                    fflutPairs.erase(ff->name);
                }

                packed_cells.insert(ci->name);
            }
        }
        flush_cells();
    }
    // Create a feed in to the carry chain
    CellInfo *make_carry_feed_in(NetInfo *carry, PortRef chain_in)
    {
        std::unique_ptr<CellInfo> feedin = create_ecp5_cell(ctx, id_CCU2C);

        feedin->params[id_INIT0] = Property(10, 16); // LUT4 = 0; LUT2 = A
        feedin->params[id_INIT1] = Property(65535, 16);
        feedin->params[id_INJECT1_0] = std::string("NO");
        feedin->params[id_INJECT1_1] = std::string("YES");

        carry->users.remove(chain_in.cell->ports.at(chain_in.port).user_idx);
        feedin->connectPort(id_A0, carry);

        NetInfo *new_carry = ctx->createNet(ctx->id(feedin->name.str(ctx) + "$COUT"));
        feedin->connectPort(id_COUT, new_carry);
        chain_in.cell->ports[chain_in.port].net = nullptr;
        chain_in.cell->ports[chain_in.port].user_idx = {};

        chain_in.cell->connectPort(chain_in.port, new_carry);

        CellInfo *feedin_ptr = feedin.get();
        IdString feedin_name = feedin->name;
        ctx->cells[feedin_name] = std::move(feedin);
        return feedin_ptr;
    }

    // Create a feed out and loop through from the carry chain
    CellInfo *make_carry_feed_out(NetInfo *carry, boost::optional<PortRef> chain_next = boost::optional<PortRef>())
    {
        std::unique_ptr<CellInfo> feedout = create_ecp5_cell(ctx, id_CCU2C);

        feedout->params[id_INIT0] = Property(0, 16);
        feedout->params[id_INIT1] = Property(10, 16); // LUT4 = 0; LUT2 = A
        feedout->params[id_INJECT1_0] = std::string("NO");
        feedout->params[id_INJECT1_1] = std::string("NO");

        PortRef carry_drv = carry->driver;
        carry->driver.cell = nullptr;
        feedout->connectPort(id_S0, carry);

        NetInfo *new_cin = ctx->createNet(ctx->id(feedout->name.str(ctx) + "$CIN"));
        new_cin->driver = carry_drv;
        carry_drv.cell->ports.at(carry_drv.port).net = new_cin;
        feedout->connectPort(id_CIN, new_cin);

        if (chain_next) {
            // Loop back into LUT4_1 for feedthrough
            feedout->connectPort(id_A1, carry);
            if (chain_next->cell && chain_next->cell->ports.at(chain_next->port).user_idx)
                carry->users.remove(chain_next->cell->ports.at(chain_next->port).user_idx);

            NetInfo *new_cout = ctx->createNet(ctx->id(feedout->name.str(ctx) + "$COUT"));
            feedout->connectPort(id_COUT, new_cout);

            chain_next->cell->ports[chain_next->port].net = nullptr;
            chain_next->cell->connectPort(chain_next->port, new_cout);
        }

        CellInfo *feedout_ptr = feedout.get();
        IdString feedout_name = feedout->name;
        ctx->cells[feedout_name] = std::move(feedout);

        return feedout_ptr;
    }

    // Split a carry chain into multiple legal chains
    std::vector<CellChain> split_carry_chain(CellChain &carryc)
    {
        bool start_of_chain = true;
        std::vector<CellChain> chains;
        const int max_length = (ctx->chip_info->width - 4) * 4 - 2;
        auto curr_cell = carryc.cells.begin();
        while (curr_cell != carryc.cells.end()) {
            CellInfo *cell = *curr_cell;
            if (start_of_chain) {
                chains.emplace_back();
                start_of_chain = false;
                if (cell->ports.at(id_CIN).net) {
                    // CIN is not constant and not part of a chain. Must feed in from fabric
                    PortRef inport;
                    inport.cell = cell;
                    inport.port = id_CIN;
                    CellInfo *feedin = make_carry_feed_in(cell->ports.at(id_CIN).net, inport);
                    chains.back().cells.push_back(feedin);
                }
            }
            chains.back().cells.push_back(cell);
            bool split_chain = int(chains.back().cells.size()) > max_length;
            if (split_chain) {
                CellInfo *passout = make_carry_feed_out(cell->ports.at(id_COUT).net);
                chains.back().cells.back() = passout;
                start_of_chain = true;
            } else {
                NetInfo *carry_net = cell->ports.at(id_COUT).net;
                bool at_end = (curr_cell == carryc.cells.end() - 1);
                if (carry_net != nullptr && (carry_net->users.entries() > 1 || at_end)) {
                    boost::optional<PortRef> nextport;
                    if (!at_end) {
                        auto next_cell = *(curr_cell + 1);
                        PortRef nextpr;
                        nextpr.cell = next_cell;
                        nextpr.port = id_CIN;
                        nextport = nextpr;
                    }
                    CellInfo *passout = make_carry_feed_out(cell->ports.at(id_COUT).net, nextport);
                    chains.back().cells.push_back(passout);
                }
                ++curr_cell;
            }
        }
        return chains;
    }

    // Pack carries and set up appropriate relative constraints
    void pack_carries()
    {
        log_info("Packing carries...\n");
        // Find all chains (including single carry cells)
        auto carry_chains = find_chains(
                ctx, [](const Context *ctx, const CellInfo *cell) { return is_carry(ctx, cell); },
                [](const Context *ctx, const CellInfo *cell) {
                    return net_driven_by(ctx, cell->ports.at(id_CIN).net, is_carry, id_COUT);
                },
                [](const Context *ctx, const CellInfo *cell) {
                    return net_only_drives(ctx, cell->ports.at(id_COUT).net, is_carry, id_CIN, false);
                },
                1);
        std::vector<CellChain> all_chains;

        // Chain splitting
        for (auto &base_chain : carry_chains) {
            if (ctx->verbose) {
                log_info("Found carry chain: \n");
                for (auto entry : base_chain.cells)
                    log_info("     %s\n", entry->name.c_str(ctx));
                log_info("\n");
            }
            std::vector<CellChain> split_chains = split_carry_chain(base_chain);
            for (auto &chain : split_chains) {
                all_chains.push_back(chain);
            }
        }

        std::vector<std::vector<CellInfo *>> packed_chains;

        // Chain packing
        std::vector<std::tuple<CellInfo *, CellInfo *, int>> ff_packing;
        for (auto &chain : all_chains) {
            int cell_count = 0;
            std::vector<CellInfo *> tile_ffs;
            std::vector<CellInfo *> packed_chain;
            for (auto &cell : chain.cells) {
                if (cell_count % 4 == 0)
                    tile_ffs.clear();
                std::unique_ptr<CellInfo> slice =
                        create_ecp5_cell(ctx, id_TRELLIS_SLICE, cell->name.str(ctx) + "$CCU2_SLICE");

                ccu2c_to_slice(ctx, cell, slice.get());

                CellInfo *ff0 = nullptr;
                NetInfo *f0net = slice->ports.at(id_F0).net;
                if (f0net != nullptr) {
                    ff0 = net_only_drives(ctx, f0net, is_ff, id_DI, false);
                    if (ff0 != nullptr && can_add_ff_to_tile(tile_ffs, ff0)) {
                        ff_packing.push_back(std::make_tuple(ff0, slice.get(), 0));
                        tile_ffs.push_back(ff0);
                        packed_cells.insert(ff0->name);
                    }
                }

                CellInfo *ff1 = nullptr;
                NetInfo *f1net = slice->ports.at(id_F1).net;
                if (f1net != nullptr) {
                    ff1 = net_only_drives(ctx, f1net, is_ff, id_DI, false);
                    if (ff1 != nullptr && (ff0 == nullptr || can_pack_ffs(ff0, ff1)) &&
                        can_add_ff_to_tile(tile_ffs, ff1)) {
                        ff_packing.push_back(std::make_tuple(ff1, slice.get(), 1));
                        tile_ffs.push_back(ff1);
                        packed_cells.insert(ff1->name);
                    }
                }
                packed_chain.push_back(slice.get());
                new_cells.push_back(std::move(slice));
                packed_cells.insert(cell->name);
                cell_count++;
            }
            packed_chains.push_back(packed_chain);
        }

        for (auto ff : ff_packing)
            ff_to_slice(ctx, std::get<0>(ff), std::get<1>(ff), std::get<2>(ff), true);

        // Relative chain placement
        for (auto &chain : packed_chains) {
            chain.at(0)->constr_abs_z = true;
            chain.at(0)->constr_z = 0;
            chain.at(0)->cluster = chain.at(0)->name;
            for (int i = 1; i < int(chain.size()); i++) {
                chain.at(i)->constr_x = (i / 4);
                chain.at(i)->constr_y = 0;
                chain.at(i)->constr_z = i % 4;
                chain.at(i)->constr_abs_z = true;
                chain.at(i)->cluster = chain.at(0)->name;
                chain.at(0)->constr_children.push_back(chain.at(i));
            }
        }

        flush_cells();
    }

    // Pack distributed RAM
    void pack_dram()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (is_dpram(ctx, ci)) {

                // Create RAMW slice
                std::unique_ptr<CellInfo> ramw_slice =
                        create_ecp5_cell(ctx, id_TRELLIS_SLICE, ci->name.str(ctx) + "$RAMW_SLICE");
                dram_to_ramw(ctx, ci, ramw_slice.get());

                // Create actual RAM slices
                std::unique_ptr<CellInfo> ram0_slice =
                        create_ecp5_cell(ctx, id_TRELLIS_SLICE, ci->name.str(ctx) + "$DPRAM0_SLICE");
                dram_to_ram_slice(ctx, ci, ram0_slice.get(), ramw_slice.get(), 0);

                std::unique_ptr<CellInfo> ram1_slice =
                        create_ecp5_cell(ctx, id_TRELLIS_SLICE, ci->name.str(ctx) + "$DPRAM1_SLICE");
                dram_to_ram_slice(ctx, ci, ram1_slice.get(), ramw_slice.get(), 1);

                // Disconnect ports of original cell after packing
                ci->disconnectPort(id_WCK);
                ci->disconnectPort(id_WRE);

                ci->disconnectPort(ctx->id("RAD[0]"));
                ci->disconnectPort(ctx->id("RAD[1]"));
                ci->disconnectPort(ctx->id("RAD[2]"));
                ci->disconnectPort(ctx->id("RAD[3]"));

                // Attempt to pack FFs into RAM slices
                std::vector<std::tuple<CellInfo *, CellInfo *, int>> ff_packing;
                std::vector<CellInfo *> tile_ffs;
                for (auto slice : {ram0_slice.get(), ram1_slice.get()}) {
                    CellInfo *ff0 = nullptr;
                    NetInfo *f0net = slice->ports.at(id_F0).net;
                    if (f0net != nullptr) {
                        ff0 = net_only_drives(ctx, f0net, is_ff, id_DI, false);
                        if (ff0 != nullptr && can_add_ff_to_tile(tile_ffs, ff0)) {
                            if (can_pack_ff_dram(slice, ff0)) {
                                ff_packing.push_back(std::make_tuple(ff0, slice, 0));
                                tile_ffs.push_back(ff0);
                                packed_cells.insert(ff0->name);
                            }
                        }
                    }

                    CellInfo *ff1 = nullptr;
                    NetInfo *f1net = slice->ports.at(id_F1).net;
                    if (f1net != nullptr) {
                        ff1 = net_only_drives(ctx, f1net, is_ff, id_DI, false);
                        if (ff1 != nullptr && (ff0 == nullptr || can_pack_ffs(ff0, ff1)) &&
                            can_add_ff_to_tile(tile_ffs, ff1)) {
                            if (can_pack_ff_dram(slice, ff1)) {
                                ff_packing.push_back(std::make_tuple(ff1, slice, 1));
                                tile_ffs.push_back(ff1);
                                packed_cells.insert(ff1->name);
                            }
                        }
                    }
                }

                for (auto ff : ff_packing)
                    ff_to_slice(ctx, std::get<0>(ff), std::get<1>(ff), std::get<2>(ff), true);

                // Setup placement constraints
                ram0_slice->constr_abs_z = true;
                ram0_slice->constr_z = 0;
                ram0_slice->cluster = ram0_slice->name;

                ram1_slice->cluster = ram0_slice->name;
                ram1_slice->constr_abs_z = true;
                ram1_slice->constr_x = 0;
                ram1_slice->constr_y = 0;
                ram1_slice->constr_z = 1;
                ram0_slice->constr_children.push_back(ram1_slice.get());

                ramw_slice->cluster = ram0_slice->name;
                ramw_slice->constr_abs_z = true;
                ramw_slice->constr_x = 0;
                ramw_slice->constr_y = 0;
                ramw_slice->constr_z = 2;
                ram0_slice->constr_children.push_back(ramw_slice.get());

                new_cells.push_back(std::move(ram0_slice));
                new_cells.push_back(std::move(ram1_slice));
                new_cells.push_back(std::move(ramw_slice));
                packed_cells.insert(ci->name);
            }
        }
        flush_cells();
    }

    // Pack LUTs that have been paired together
    void pack_lut_pairs()
    {
        log_info("Packing paired LUTs into a SLICE...\n");
        for (auto pair : lutPairs) {
            CellInfo *lut0 = ctx->cells.at(pair.first).get();
            CellInfo *lut1 = ctx->cells.at(pair.second).get();
            std::unique_ptr<CellInfo> slice = create_ecp5_cell(ctx, id_TRELLIS_SLICE, lut0->name.str(ctx) + "_SLICE");

            lut_to_slice(ctx, lut0, slice.get(), 0);
            lut_to_slice(ctx, lut1, slice.get(), 1);

            auto ff0 = lutffPairs.find(lut0->name);

            if (ff0 != lutffPairs.end()) {
                ff_to_slice(ctx, ctx->cells.at(ff0->second).get(), slice.get(), 0, true);
                packed_cells.insert(ff0->second);
                fflutPairs.erase(ff0->second);
                lutffPairs.erase(lut0->name);
            }

            auto ff1 = lutffPairs.find(lut1->name);

            if (ff1 != lutffPairs.end()) {
                ff_to_slice(ctx, ctx->cells.at(ff1->second).get(), slice.get(), 1, true);
                packed_cells.insert(ff1->second);
                fflutPairs.erase(ff1->second);
                lutffPairs.erase(lut1->name);
            }

            new_cells.push_back(std::move(slice));
            packed_cells.insert(lut0->name);
            packed_cells.insert(lut1->name);
        }
        flush_cells();
    }

    // Pack single LUTs that weren't paired into their own slice,
    // with an optional FF also
    void pack_remaining_luts()
    {
        log_info("Packing unpaired LUTs into a SLICE...\n");
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (is_lut(ctx, ci)) {
                std::unique_ptr<CellInfo> slice = create_ecp5_cell(ctx, id_TRELLIS_SLICE, ci->name.str(ctx) + "_SLICE");
                lut_to_slice(ctx, ci, slice.get(), 1);
                auto ff = lutffPairs.find(ci->name);

                if (ff != lutffPairs.end()) {
                    ff_to_slice(ctx, ctx->cells.at(ff->second).get(), slice.get(), 1, true);
                    packed_cells.insert(ff->second);
                    fflutPairs.erase(ff->second);
                    lutffPairs.erase(ci->name);
                }

                new_cells.push_back(std::move(slice));
                packed_cells.insert(ci->name);
            }
        }
        flush_cells();
    }

    // Find a cell that meets some criteria near an origin cell
    // Used for packing an FF into a nearby SLICE
    template <typename TFunc> CellInfo *find_nearby_cell(CellInfo *origin, TFunc Func)
    {
        pool<CellInfo *, hash_ptr_ops> visited_cells;
        std::queue<CellInfo *> to_visit;
        visited_cells.insert(origin);
        to_visit.push(origin);
        int iter = 0;
        while (!to_visit.empty() && iter < 10000) {
            CellInfo *cursor = to_visit.front();
            to_visit.pop();
            if (Func(cursor))
                return cursor;
            for (const auto &port : cursor->ports) {
                NetInfo *pn = port.second.net;
                if (pn == nullptr)
                    continue;
                // Skip high-fanout nets that are unlikely to be relevant
                if (pn->users.entries() > 25)
                    continue;
                // Add other ports on this net if not already visited
                auto visit_port = [&](const PortRef &port) {
                    if (port.cell == nullptr)
                        return;
                    if (visited_cells.count(port.cell))
                        return;
                    // If not already visited; add the cell of this port to the queue
                    to_visit.push(port.cell);
                    visited_cells.insert(port.cell);
                };
                visit_port(pn->driver);
                for (const auto &usr : pn->users)
                    visit_port(usr);
            }
            ++iter;
        }
        return nullptr;
    }

    // Pack flipflops that weren't paired with a LUT
    float dense_pack_mode_thresh = 0.95f;
    void pack_remaining_ffs()
    {
        // Enter dense flipflop packing mode once utilisation exceeds a threshold (default: 95%)
        int used_slices = 0;
        for (auto &cell : ctx->cells)
            if (cell.second->type == id_TRELLIS_SLICE)
                ++used_slices;

        log_info("Packing unpaired FFs into a SLICE...\n");
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (is_ff(ctx, ci)) {
                bool pack_dense = used_slices > (dense_pack_mode_thresh * available_slices);
                bool requires_m = ci->getPort(id_M) != nullptr;
                if (pack_dense && !requires_m) {
                    // If dense packing threshold exceeded; always try and pack the FF into an existing slice
                    // Find a SLICE with space "near" the flipflop in the netlist
                    std::vector<CellInfo *> ltile;
                    CellInfo *target = find_nearby_cell(ci, [&](CellInfo *cursor) {
                        if (cursor->type != id_TRELLIS_SLICE)
                            return false;
                        if (cursor->cluster != ClusterId()) {
                            auto &constr_children = ctx->cells.at(cursor->cluster)->constr_children;
                            // Skip big chains for performance
                            if (constr_children.size() > 8)
                                return false;
                            // Have to check the whole of the tile for legality when dealing with chains, not just slice
                            ltile.clear();
                            if (cursor->cluster != cursor->name)
                                ltile.push_back(ctx->cells.at(cursor->cluster).get());
                            else
                                ltile.push_back(cursor);
                            for (auto c : constr_children)
                                ltile.push_back(c);
                            if (!can_add_ff_to_tile(ltile, cursor))
                                return false;
                        }
                        if (!can_add_ff_to_slice(cursor, ci))
                            return false;
                        for (int i = 0; i < 2; i++)
                            if (is_ff_available(cursor, i))
                                return true;
                        return false;
                    });

                    // If found, add the FF to this slice instead of creating a new one
                    if (target != nullptr) {
                        for (int i = 0; i < 2; i++) {
                            if (is_ff_available(target, i)) {
                                ff_to_slice(ctx, ci, target, i, false);
                                goto ff_packed;
                            }
                        }
                    }

                    if (false) {
                    ff_packed:
                        packed_cells.insert(ci->name);
                        continue;
                    }
                }

                std::unique_ptr<CellInfo> slice = create_ecp5_cell(ctx, id_TRELLIS_SLICE, ci->name.str(ctx) + "_SLICE");
                ff_to_slice(ctx, ci, slice.get(), 0, false);
                new_cells.push_back(std::move(slice));
                ++used_slices;
                packed_cells.insert(ci->name);
            }
        }
        flush_cells();
    }

    int make_init_with_const_input(int init, int input, bool value)
    {
        int new_init = 0;
        for (int i = 0; i < 16; i++) {
            if (((i >> input) & 0x1) != value) {
                int other_i = (i & (~(1 << input))) | (value << input);
                if ((init >> other_i) & 0x1)
                    new_init |= (1 << i);
            } else {
                if ((init >> i) & 0x1)
                    new_init |= (1 << i);
            }
        }
        return new_init;
    }

    void set_lut_input_constant(CellInfo *cell, IdString input, bool value)
    {
        int index = std::string("ABCD").find(input.str(ctx));
        int init = int_or_default(cell->params, id_INIT);
        int new_init = make_init_with_const_input(init, index, value);
        cell->params[id_INIT] = Property(new_init, 16);
        cell->ports.at(input).net = nullptr;
    }

    void set_ccu2c_input_constant(CellInfo *cell, IdString input, bool value)
    {
        std::string input_str = input.str(ctx);
        int lut = std::stoi(input_str.substr(1));
        int index = std::string("ABCD").find(input_str[0]);
        int init = int_or_default(cell->params, ctx->id("INIT" + std::to_string(lut)));
        int new_init = make_init_with_const_input(init, index, value);
        cell->params[ctx->id("INIT" + std::to_string(lut))] = Property(new_init, 16);
        cell->ports.at(input).net = nullptr;
    }

    bool is_ccu2c_port_high(CellInfo *cell, IdString input)
    {
        if (!cell->ports.count(input))
            return true; // disconnected port is high
        if (cell->ports.at(input).net == nullptr || cell->ports.at(input).net->name == ctx->id("$PACKER_VCC_NET"))
            return true; // disconnected or tied-high port
        if (cell->ports.at(input).net->driver.cell != nullptr && cell->ports.at(input).net->driver.cell->type == id_VCC)
            return true; // pre-pack high
        return false;
    }

    // Merge a net into a constant net
    void set_net_constant(const Context *ctx, NetInfo *orig, NetInfo *constnet, bool constval)
    {
        orig->driver.cell = nullptr;
        for (auto user : orig->users) {
            if (user.cell != nullptr) {
                CellInfo *uc = user.cell;
                if (ctx->verbose)
                    log_info("%s user %s\n", orig->name.c_str(ctx), uc->name.c_str(ctx));
                if (is_lut(ctx, uc)) {
                    set_lut_input_constant(uc, user.port, constval);
                } else if (is_ff(ctx, uc) && user.port == id_CE) {
                    uc->params[id_CEMUX] = std::string(constval ? "1" : "0");
                    uc->ports[user.port].net = nullptr;
                } else if (is_carry(ctx, uc)) {
                    if (constval &&
                        (user.port == id_A0 || user.port == id_A1 || user.port == id_B0 || user.port == id_B1 ||
                         user.port == id_C0 || user.port == id_C1 || user.port == id_D0 || user.port == id_D1)) {
                        // Input tied high, nothing special to do (bitstream gen will auto-enable tie-high)
                        uc->ports[user.port].net = nullptr;
                    } else if (!constval) {
                        if (user.port == id_A0 || user.port == id_A1 || user.port == id_B0 || user.port == id_B1) {
                            // These inputs can be switched to tie-high without consequence
                            set_ccu2c_input_constant(uc, user.port, constval);
                        } else if (user.port == id_C0 && is_ccu2c_port_high(uc, id_D0)) {
                            // Partner must be tied high
                            set_ccu2c_input_constant(uc, user.port, constval);
                        } else if (user.port == id_D0 && is_ccu2c_port_high(uc, id_C0)) {
                            // Partner must be tied high
                            set_ccu2c_input_constant(uc, user.port, constval);
                        } else if (user.port == id_C1 && is_ccu2c_port_high(uc, id_D1)) {
                            // Partner must be tied high
                            set_ccu2c_input_constant(uc, user.port, constval);
                        } else if (user.port == id_D1 && is_ccu2c_port_high(uc, id_C1)) {
                            // Partner must be tied high
                            set_ccu2c_input_constant(uc, user.port, constval);
                        } else {
                            // Not allowed to change to a tie-high
                            uc->ports[user.port].net = constnet;
                            uc->ports[user.port].user_idx = constnet->users.add(user);
                        }
                    } else {
                        uc->ports[user.port].net = constnet;
                        uc->ports[user.port].user_idx = constnet->users.add(user);
                    }
                } else if (is_ff(ctx, uc) && user.port == id_LSR &&
                           ((!constval && str_or_default(uc->params, id_LSRMUX, "LSR") == "LSR") ||
                            (constval && str_or_default(uc->params, id_LSRMUX, "LSR") == "INV"))) {
                    uc->ports[user.port].net = nullptr;
                } else if (uc->type == id_DP16KD) {
                    if (user.port == id_CLKA || user.port == id_CLKB || user.port == id_RSTA || user.port == id_RSTB ||
                        user.port == id_WEA || user.port == id_WEB || user.port == id_CEA || user.port == id_CEB ||
                        user.port == id_OCEA || user.port == id_OCEB || user.port == id_CSA0 || user.port == id_CSA1 ||
                        user.port == id_CSA2 || user.port == id_CSB0 || user.port == id_CSB1 || user.port == id_CSB2) {
                        // Connect to CIB CLK, LSR or CE. Default state is 1
                        uc->params[ctx->id(user.port.str(ctx) + "MUX")] = constval ? user.port.str(ctx) : "INV";
                    } else {
                        // Connected to CIB ABCD. Default state is bitstream configurable
                        uc->params[ctx->id(user.port.str(ctx) + "MUX")] = std::string(constval ? "1" : "0");
                    }
                    uc->ports[user.port].net = nullptr;
                } else if (uc->type == id_ALU54B || uc->type == id_MULT18X18D) {
                    if (user.port.str(ctx).substr(0, 3) == "CLK" || user.port.str(ctx).substr(0, 2) == "CE" ||
                        user.port.str(ctx).substr(0, 3) == "RST" || user.port.str(ctx).substr(0, 3) == "SRO" ||
                        user.port.str(ctx).substr(0, 3) == "SRI" || user.port.str(ctx).substr(0, 2) == "RO" ||
                        user.port.str(ctx).substr(0, 2) == "MA" || user.port.str(ctx).substr(0, 2) == "MB" ||
                        user.port.str(ctx).substr(0, 3) == "CFB" || user.port.str(ctx).substr(0, 3) == "CIN" ||
                        user.port.str(ctx).substr(0, 6) == "SOURCE" || user.port.str(ctx).substr(0, 6) == "SIGNED" ||
                        user.port.str(ctx).substr(0, 2) == "OP") {
                        uc->ports[user.port].net = constnet;
                        uc->ports[user.port].user_idx = constnet->users.add(user);
                    } else {
                        // Connected to CIB ABCD. Default state is bitstream configurable
                        uc->params[ctx->id(user.port.str(ctx) + "MUX")] = std::string(constval ? "1" : "0");
                        uc->ports[user.port].net = nullptr;
                    }
                } else {
                    uc->ports[user.port].net = constnet;
                    uc->ports[user.port].user_idx = constnet->users.add(user);
                }
            }
        }
        orig->users.clear();
    }

    // Pack constants (simple implementation)
    void pack_constants()
    {
        log_info("Packing constants..\n");

        std::unique_ptr<CellInfo> gnd_cell = create_ecp5_cell(ctx, id_LUT4, "$PACKER_GND");
        gnd_cell->params[id_INIT] = Property(0, 16);
        auto gnd_net = std::make_unique<NetInfo>(ctx->id("$PACKER_GND_NET"));
        gnd_net->driver.cell = gnd_cell.get();
        gnd_net->driver.port = id_Z;
        gnd_cell->ports.at(id_Z).net = gnd_net.get();

        std::unique_ptr<CellInfo> vcc_cell = create_ecp5_cell(ctx, id_LUT4, "$PACKER_VCC");
        vcc_cell->params[id_INIT] = Property(65535, 16);
        auto vcc_net = std::make_unique<NetInfo>(ctx->id("$PACKER_VCC_NET"));
        vcc_net->driver.cell = vcc_cell.get();
        vcc_net->driver.port = id_Z;
        vcc_cell->ports.at(id_Z).net = vcc_net.get();

        std::vector<IdString> dead_nets;

        bool gnd_used = false, vcc_used = false;

        for (auto &net : ctx->nets) {
            NetInfo *ni = net.second.get();
            if (ni->driver.cell != nullptr && ni->driver.cell->type == id_GND) {
                IdString drv_cell = ni->driver.cell->name;
                set_net_constant(ctx, ni, gnd_net.get(), false);
                gnd_used = true;
                dead_nets.push_back(net.first);
                ctx->cells.erase(drv_cell);
            } else if (ni->driver.cell != nullptr && ni->driver.cell->type == id_VCC) {
                IdString drv_cell = ni->driver.cell->name;
                set_net_constant(ctx, ni, vcc_net.get(), true);
                vcc_used = true;
                dead_nets.push_back(net.first);
                ctx->cells.erase(drv_cell);
            }
        }

        if (gnd_used) {
            ctx->cells[gnd_cell->name] = std::move(gnd_cell);
            ctx->nets[gnd_net->name] = std::move(gnd_net);
        }
        if (vcc_used) {
            ctx->cells[vcc_cell->name] = std::move(vcc_cell);
            ctx->nets[vcc_net->name] = std::move(vcc_net);
        }

        for (auto dn : dead_nets) {
            ctx->nets.erase(dn);
        }
    }

    void autocreate_empty_port(CellInfo *cell, IdString port)
    {
        if (!cell->ports.count(port)) {
            cell->ports[port].name = port;
            cell->ports[port].net = nullptr;
            cell->ports[port].type = PORT_IN;
        }
    }

    // Pack EBR
    void pack_ebr()
    {
        // Autoincrement WID (starting from 3 seems to match vendor behaviour?)
        int wid = 3;
        auto rename_bus = [&](CellInfo *c, const std::string &oldname, const std::string &newname, int width,
                              int oldoffset, int newoffset) {
            for (int i = 0; i < width; i++)
                c->renamePort(ctx->id(oldname + std::to_string(i + oldoffset)),
                              ctx->id(newname + std::to_string(i + newoffset)));
        };
        auto rename_param = [&](CellInfo *c, const std::string &oldname, const std::string &newname) {
            IdString o = ctx->id(oldname), n = ctx->id(newname);
            if (!c->params.count(o))
                return;
            c->params[n] = c->params[o];
            c->params.erase(o);
        };
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            // Convert 36-bit PDP RAMs to regular 18-bit DP ones that match the Bel
            if (ci->type == id_PDPW16KD) {
                ci->params[id_DATA_WIDTH_A] = 36; // force PDP mode
                ci->params.erase(id_DATA_WIDTH_W);
                rename_bus(ci, "BE", "ADA", 4, 0, 0);
                rename_bus(ci, "ADW", "ADA", 9, 0, 5);
                rename_bus(ci, "ADR", "ADB", 14, 0, 0);
                rename_bus(ci, "CSW", "CSA", 3, 0, 0);
                rename_bus(ci, "CSR", "CSB", 3, 0, 0);
                rename_bus(ci, "DI", "DIA", 18, 0, 0);
                rename_bus(ci, "DI", "DIB", 18, 18, 0);
                rename_bus(ci, "DO", "DOA", 18, 18, 0);
                rename_bus(ci, "DO", "DOB", 18, 0, 0);
                ci->renamePort(id_CLKW, id_CLKA);
                ci->renamePort(id_CLKR, id_CLKB);
                ci->renamePort(id_CEW, id_CEA);
                ci->renamePort(id_CER, id_CEB);
                ci->renamePort(id_OCER, id_OCEB);
                rename_param(ci, "CLKWMUX", "CLKAMUX");
                if (str_or_default(ci->params, id_CLKAMUX) == "CLKW")
                    ci->params[id_CLKAMUX] = std::string("CLKA");
                if (str_or_default(ci->params, id_CLKBMUX) == "CLKR")
                    ci->params[id_CLKBMUX] = std::string("CLKB");
                rename_param(ci, "CLKRMUX", "CLKRMUX");
                rename_param(ci, "CSDECODE_W", "CSDECODE_A");
                rename_param(ci, "CSDECODE_R", "CSDECODE_B");
                std::string outreg = str_or_default(ci->params, id_REGMODE, "NOREG");
                ci->params[id_REGMODE_A] = outreg;
                ci->params[id_REGMODE_B] = outreg;
                ci->params.erase(id_REGMODE);
                rename_param(ci, "DATA_WIDTH_R", "DATA_WIDTH_B");
                if (ci->ports.count(id_RST)) {
                    autocreate_empty_port(ci, id_RSTA);
                    autocreate_empty_port(ci, id_RSTB);
                    NetInfo *rst = ci->ports.at(id_RST).net;
                    ci->connectPort(id_RSTA, rst);
                    ci->connectPort(id_RSTB, rst);
                    ci->disconnectPort(id_RST);
                    ci->ports.erase(id_RST);
                }
                ci->type = id_DP16KD;
            }
        }
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_DP16KD) {
                // Add ports, even if disconnected, to ensure correct tie-offs
                for (int i = 0; i < 14; i++) {
                    autocreate_empty_port(ci, ctx->id("ADA" + std::to_string(i)));
                    autocreate_empty_port(ci, ctx->id("ADB" + std::to_string(i)));
                }
                for (int i = 0; i < 18; i++) {
                    autocreate_empty_port(ci, ctx->id("DIA" + std::to_string(i)));
                    autocreate_empty_port(ci, ctx->id("DIB" + std::to_string(i)));
                }
                for (int i = 0; i < 3; i++) {
                    autocreate_empty_port(ci, ctx->id("CSA" + std::to_string(i)));
                    autocreate_empty_port(ci, ctx->id("CSB" + std::to_string(i)));
                }
                for (int i = 0; i < 3; i++) {
                    autocreate_empty_port(ci, ctx->id("CSA" + std::to_string(i)));
                    autocreate_empty_port(ci, ctx->id("CSB" + std::to_string(i)));
                }

                autocreate_empty_port(ci, id_CLKA);
                autocreate_empty_port(ci, id_CEA);
                autocreate_empty_port(ci, id_OCEA);
                autocreate_empty_port(ci, id_WEA);
                autocreate_empty_port(ci, id_RSTA);

                autocreate_empty_port(ci, id_CLKB);
                autocreate_empty_port(ci, id_CEB);
                autocreate_empty_port(ci, id_OCEB);
                autocreate_empty_port(ci, id_WEB);
                autocreate_empty_port(ci, id_RSTB);

                ci->attrs[id_WID] = wid++;
            }
        }
    }

    // Pack DSPs
    void pack_dsps()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_MULT18X18D) {
                // Add ports, even if disconnected, to ensure correct tie-offs
                for (auto sig : {"CLK", "CE", "RST"})
                    for (int i = 0; i < 4; i++)
                        autocreate_empty_port(ci, ctx->id(sig + std::to_string(i)));
                for (auto sig : {"SIGNED", "SOURCE"})
                    for (auto c : {"A", "B"})
                        autocreate_empty_port(ci, ctx->id(sig + std::string(c)));
                for (auto port : {"A", "B", "C"})
                    for (int i = 0; i < 18; i++)
                        autocreate_empty_port(ci, ctx->id(port + std::to_string(i)));
                for (auto port : {"SRIA", "SRIB"})
                    for (int i = 0; i < 18; i++)
                        autocreate_empty_port(ci, ctx->id(port + std::to_string(i)));
            } else if (ci->type == id_ALU54B) {
                for (auto sig : {"CLK", "CE", "RST"})
                    for (int i = 0; i < 4; i++)
                        autocreate_empty_port(ci, ctx->id(sig + std::to_string(i)));
                autocreate_empty_port(ci, id_SIGNEDIA);
                autocreate_empty_port(ci, id_SIGNEDIB);
                autocreate_empty_port(ci, id_SIGNEDCIN);
                for (auto port : {"A", "B", "MA", "MB"})
                    for (int i = 0; i < 36; i++)
                        autocreate_empty_port(ci, ctx->id(port + std::to_string(i)));
                for (auto port : {"C", "CFB", "CIN"})
                    for (int i = 0; i < 54; i++)
                        autocreate_empty_port(ci, ctx->id(port + std::to_string(i)));
                for (int i = 0; i < 11; i++)
                    autocreate_empty_port(ci, ctx->id("OP" + std::to_string(i)));

                // Find the MULT18X18Ds feeding this ALU54B's MA and MB inputs.
                CellInfo *mult_a = nullptr;
                CellInfo *mult_b = nullptr;
                for (auto port : {id_MA0, id_MB0}) {
                    CellInfo *mult = net_driven_by(
                            ctx, ci->ports.at(port).net,
                            [](const Context *ctx, const CellInfo *cell) { return cell->type == id_MULT18X18D; },
                            id_P0);

                    // We'll handle the mult not existing in check_alu below.
                    if (mult == nullptr)
                        break;

                    // Set relative constraint depending on ALU port.
                    if (port == id_MA0) {
                        mult->constr_x = mult->constr_z = -3;
                        mult_a = mult;
                    } else if (port == id_MB0) {
                        mult->constr_x = mult->constr_z = -2;
                        mult_b = mult;
                    }
                    mult->constr_y = 0;
                    mult->cluster = ci->name;
                    ci->constr_x = 0;
                    ci->constr_y = 0;
                    ci->constr_z = 0;
                    ci->cluster = ci->name;
                    ci->constr_children.push_back(mult);
                    log_info("DSP: Constraining MULT18X18D '%s' to ALU54B '%s' port %s\n", mult->name.c_str(ctx),
                             cell.first.c_str(ctx), ctx->nameOf(port));
                }

                // Check existance of, and connectivity to, each MULT.
                check_alu(ci, mult_a, mult_b);
            }
        }
    }

    // Check ALU54B is correctly connected to two MULT18X18Ds.
    void check_alu(CellInfo *alu, CellInfo *mult_a, CellInfo *mult_b)
    {
        // MULT18X18Ds must be detected on both inputs.
        if (mult_a == nullptr) {
            log_error("No MULT18X18D found connected to ALU54B '%s' port A\n", alu->name.c_str(ctx));
        } else if (mult_b == nullptr) {
            log_error("No MULT18X18D found connected to ALU54B '%s' port B\n", alu->name.c_str(ctx));
        }

        // Placement doesn't work if only one or the other of
        // the ALU and MULTs have a BEL specified.
        auto alu_has_bel = alu->attrs.count(id_BEL);
        for (auto mult : {mult_a, mult_b}) {
            auto mult_has_bel = mult->attrs.count(id_BEL);
            if (alu_has_bel && !mult_has_bel) {
                log_error("ALU54B '%s' has a fixed BEL specified, but connected "
                          "MULT18X18D '%s' does not, specify both or neither.\n",
                          alu->name.c_str(ctx), mult->name.c_str(ctx));
            } else if (!alu_has_bel && mult_has_bel) {
                log_error("ALU54B '%s' does not have a fixed BEL specified, but "
                          "connected MULT18X18D '%s' does, specify both or neither.\n",
                          alu->name.c_str(ctx), mult->name.c_str(ctx));
            }
        }

        // Cannot have MULT OUTPUT_CLK set when connected to an ALU unless
        // MULT_BYPASS is also enabled.
        for (auto mult : {mult_a, mult_b}) {
            if (str_or_default(mult->params, id_REG_OUTPUT_CLK, "NONE") != "NONE" &&
                str_or_default(mult->params, id_MULT_BYPASS, "DISABLED") != "ENABLED") {
                log_error("MULT18X18D '%s' REG_OUTPUT_CLK must be NONE when driving ALU without MULT_BYPASS\n",
                          mult->name.c_str(ctx));
            }
        }

        // SIGNEDIA and SIGNEDIB inputs must be connected to SIGNEDP output.
        NetInfo *net = alu->ports.at(id_SIGNEDIA).net;
        if (net == nullptr || net->driver.cell != mult_a || net->driver.port != id_SIGNEDP) {
            log_error("ALU54B '%s' input SIGNEDIA must be driven by SIGNEDP of"
                      " MULT18X18D '%s'\n",
                      alu->name.c_str(ctx), mult_a->name.c_str(ctx));
        }
        net = alu->ports.at(id_SIGNEDIB).net;
        if (net == nullptr || net->driver.cell != mult_b || net->driver.port != id_SIGNEDP) {
            log_error("ALU54B '%s' input SIGNEDIB must be driven by SIGNEDP of"
                      " MULT18X18D '%s'\n",
                      alu->name.c_str(ctx), mult_b->name.c_str(ctx));
        }

        // All A and B inputs must be connected to ROA/ROB outputs,
        // and all MA and MB inputs must be connected to P outputs.
        for (int i = 0; i < 36; i++) {
            IdString mult_port;
            if (i < 18)
                mult_port = ctx->id(std::string("ROA") + std::to_string(i));
            else
                mult_port = ctx->id(std::string("ROB") + std::to_string(i - 18));

            IdString alu_port = ctx->id(std::string("A") + std::to_string(i));
            net = alu->ports.at(alu_port).net;
            if (net == nullptr || net->driver.cell != mult_a || net->driver.port != mult_port) {
                log_error("ALU54B '%s' input %s must be driven by %s of MULT18X18D '%s'\n", alu->name.c_str(ctx),
                          alu_port.c_str(ctx), mult_port.c_str(ctx), mult_a->name.c_str(ctx));
            }

            alu_port = ctx->id(std::string("B") + std::to_string(i));
            net = alu->ports.at(alu_port).net;
            if (net == nullptr || net->driver.cell != mult_b || net->driver.port != mult_port) {
                log_error("ALU54B '%s' input %s must be driven by %s of MULT18X18D '%s'\n", alu->name.c_str(ctx),
                          alu_port.c_str(ctx), mult_port.c_str(ctx), mult_b->name.c_str(ctx));
            }

            mult_port = ctx->id(std::string("P") + std::to_string(i));

            alu_port = ctx->id(std::string("MA") + std::to_string(i));
            net = alu->ports.at(alu_port).net;
            if (net == nullptr || net->driver.cell != mult_a || net->driver.port != mult_port) {
                log_error("ALU54B '%s' input %s must be driven by %s of MULT18X18D '%s'\n", alu->name.c_str(ctx),
                          alu_port.c_str(ctx), mult_port.c_str(ctx), mult_a->name.c_str(ctx));
            }

            alu_port = ctx->id(std::string("MB") + std::to_string(i));
            net = alu->ports.at(alu_port).net;
            if (net == nullptr || net->driver.cell != mult_b || net->driver.port != mult_port) {
                log_error("ALU54B '%s' input %s must be driven by %s of MULT18X18D '%s'\n", alu->name.c_str(ctx),
                          alu_port.c_str(ctx), mult_port.c_str(ctx), mult_b->name.c_str(ctx));
            }
        }
    }

    // "Pack" DCUs
    void pack_dcus()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_DCUA) {
                if (ci->attrs.count(id_LOC)) {
                    std::string loc = ci->attrs.at(id_LOC).as_string();
                    if (loc == "DCU0" &&
                        (ctx->args.type == ArchArgs::LFE5UM_25F || ctx->args.type == ArchArgs::LFE5UM5G_25F))
                        ci->attrs[id_BEL] = std::string("X42/Y50/DCU");
                    else if (loc == "DCU0" &&
                             (ctx->args.type == ArchArgs::LFE5UM_45F || ctx->args.type == ArchArgs::LFE5UM5G_45F))
                        ci->attrs[id_BEL] = std::string("X42/Y71/DCU");
                    else if (loc == "DCU1" &&
                             (ctx->args.type == ArchArgs::LFE5UM_45F || ctx->args.type == ArchArgs::LFE5UM5G_45F))
                        ci->attrs[id_BEL] = std::string("X69/Y71/DCU");
                    else if (loc == "DCU0" &&
                             (ctx->args.type == ArchArgs::LFE5UM_85F || ctx->args.type == ArchArgs::LFE5UM5G_85F))
                        ci->attrs[id_BEL] = std::string("X46/Y95/DCU");
                    else if (loc == "DCU1" &&
                             (ctx->args.type == ArchArgs::LFE5UM_85F || ctx->args.type == ArchArgs::LFE5UM5G_85F))
                        ci->attrs[id_BEL] = std::string("X71/Y95/DCU");
                    else
                        log_error("no DCU location '%s' in device '%s'\n", loc.c_str(), ctx->getChipName().c_str());
                }
                if (!ci->attrs.count(id_BEL))
                    log_error("DCU must be constrained to a Bel!\n");
                // Empty port auto-creation to generate correct tie-downs
                BelId exemplar_bel;
                for (auto bel : ctx->getBels()) {
                    if (ctx->getBelType(bel) == id_DCUA) {
                        exemplar_bel = bel;
                        break;
                    }
                }
                NPNR_ASSERT(exemplar_bel != BelId());
                for (auto pin : ctx->getBelPins(exemplar_bel))
                    if (ctx->getBelPinType(exemplar_bel, pin) == PORT_IN)
                        autocreate_empty_port(ci, pin);
                // Disconnect these ports if connected to constant to prevent routing failure
                for (auto ndport : {id_D_TXBIT_CLKP_FROM_ND, id_D_TXBIT_CLKN_FROM_ND, id_D_SYNC_ND,
                                    id_D_TXPLL_LOL_FROM_ND, id_CH0_HDINN, id_CH0_HDINP, id_CH1_HDINN, id_CH1_HDINP}) {
                    const NetInfo *net = ci->getPort(ndport);
                    if (net == nullptr || net->driver.cell == nullptr)
                        continue;
                    IdString ct = net->driver.cell->type;
                    if (ct == id_GND || ct == id_VCC) {
                        ci->disconnectPort(ndport);
                        ci->ports.erase(ndport);
                    }
                }
            }
        }
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_EXTREFB) {
                const NetInfo *refo = net_or_nullptr(ci, id_REFCLKO);
                CellInfo *dcu = nullptr;
                std::string loc_bel = std::string("NONE");
                std::string dcu_bel = std::string("NONE");
                if (ci->attrs.count(id_LOC)) {
                    std::string loc = ci->attrs.at(id_LOC).as_string();
                    if (loc == "EXTREF0" &&
                        (ctx->args.type == ArchArgs::LFE5UM_25F || ctx->args.type == ArchArgs::LFE5UM5G_25F))
                        loc_bel = std::string("X42/Y50/EXTREF");
                    else if (loc == "EXTREF0" &&
                             (ctx->args.type == ArchArgs::LFE5UM_45F || ctx->args.type == ArchArgs::LFE5UM5G_45F))
                        loc_bel = std::string("X42/Y71/EXTREF");
                    else if (loc == "EXTREF1" &&
                             (ctx->args.type == ArchArgs::LFE5UM_45F || ctx->args.type == ArchArgs::LFE5UM5G_45F))
                        loc_bel = std::string("X69/Y71/EXTREF");
                    else if (loc == "EXTREF0" &&
                             (ctx->args.type == ArchArgs::LFE5UM_85F || ctx->args.type == ArchArgs::LFE5UM5G_85F))
                        loc_bel = std::string("X46/Y95/EXTREF");
                    else if (loc == "EXTREF1" &&
                             (ctx->args.type == ArchArgs::LFE5UM_85F || ctx->args.type == ArchArgs::LFE5UM5G_85F))
                        loc_bel = std::string("X71/Y95/EXTREF");
                }
                if (refo == nullptr)
                    log_error("EXTREFB REFCLKO must not be unconnected\n");
                for (auto user : refo->users) {
                    if (user.cell->type != id_DCUA)
                        continue;
                    if (dcu != nullptr && dcu != user.cell)
                        log_error("EXTREFB REFCLKO must only drive a single DCUA\n");
                    dcu = user.cell;
                }
                if (dcu != nullptr) {
                    if (!dcu->attrs.count(id_BEL))
                        log_error("DCU must be constrained to a Bel!\n");
                    dcu_bel = dcu->attrs.at(id_BEL).as_string();
                    NPNR_ASSERT(dcu_bel.substr(dcu_bel.length() - 3) == "DCU");
                    dcu_bel.replace(dcu_bel.length() - 3, 3, "EXTREF");
                }
                if (dcu_bel != loc_bel) {
                    if (dcu_bel == "NONE" && loc_bel == "NONE") {
                        log_error("EXTREFB has neither a LOC or a directly associated DCUA\n");
                    } else if (dcu_bel == "NONE") {
                        ci->attrs[id_BEL] = loc_bel;
                        dcu_bel = loc_bel;
                    } else if (loc_bel == "NONE") {
                        ci->attrs[id_BEL] = dcu_bel;
                    } else {
                        log_error("EXTREFB has conflicting LOC '%s' and associated DCUA '%s'\n", loc_bel.c_str(),
                                  dcu_bel.c_str());
                    }
                } else {
                    if (dcu_bel == "NONE")
                        log_error("EXTREFB has no LOC or associated DCUA\n");
                    ci->attrs[id_BEL] = dcu_bel;
                }
            } else if (ci->type == id_PCSCLKDIV) {
                const NetInfo *clki = net_or_nullptr(ci, id_CLKI);
                if (clki != nullptr && clki->driver.cell != nullptr && clki->driver.cell->type == id_DCUA) {
                    CellInfo *dcu = clki->driver.cell;
                    if (!dcu->attrs.count(id_BEL))
                        log_error("DCU must be constrained to a Bel!\n");
                    BelId bel = ctx->getBelByNameStr(dcu->attrs.at(id_BEL).as_string());
                    if (bel == BelId())
                        log_error("Invalid DCU bel '%s'\n", dcu->attrs.at(id_BEL).c_str());
                    Loc loc = ctx->getBelLocation(bel);
                    // DCU0 -> CLKDIV z=0; DCU1 -> CLKDIV z=1
                    ci->constr_abs_z = true;
                    ci->constr_z = (loc.x >= 69) ? 1 : 0;
                }
            }
        }
    }

    // Miscellaneous packer tasks
    void pack_misc()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_USRMCLK) {
                ci->renamePort(id_USRMCLKI, id_PADDO);
                ci->renamePort(id_USRMCLKTS, id_PADDT);
                ci->renamePort(id_USRMCLKO, id_PADDI);
            } else if (ci->type == id_GSR || ci->type == id_SGSR) {
                ci->params[id_MODE] = std::string("ACTIVE_LOW");
                ci->params[id_SYNCMODE] = ci->type == id_SGSR ? std::string("SYNC") : std::string("ASYNC");
                ci->type = id_GSR;
                for (BelId bel : ctx->getBels()) {
                    if (ctx->getBelType(bel) != id_GSR)
                        continue;
                    ci->attrs[id_BEL] = ctx->getBelName(bel).str(ctx);
                    ctx->gsrclk_wire = ctx->getBelPinWire(bel, id_CLK);
                }
            }
        }
    }

    // Preplace PLL
    void preplace_plls()
    {
        std::set<BelId> available_plls;
        for (auto bel : ctx->getBels()) {
            if (ctx->getBelType(bel) == id_EHXPLLL && ctx->checkBelAvail(bel))
                available_plls.insert(bel);
        }
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_EHXPLLL && ci->attrs.count(id_BEL))
                available_plls.erase(ctx->getBelByNameStr(ci->attrs.at(id_BEL).as_string()));
        }
        // Place PLL connected to fixed drivers such as IO close to their source
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_EHXPLLL && !ci->attrs.count(id_BEL)) {
                const NetInfo *drivernet = net_or_nullptr(ci, id_CLKI);
                if (drivernet == nullptr || drivernet->driver.cell == nullptr)
                    continue;
                const CellInfo *drivercell = drivernet->driver.cell;
                if (!drivercell->attrs.count(id_BEL))
                    continue;
                BelId drvbel = ctx->getBelByNameStr(drivercell->attrs.at(id_BEL).as_string());
                Loc drvloc = ctx->getBelLocation(drvbel);
                BelId closest_pll;
                int closest_distance = std::numeric_limits<int>::max();
                for (auto bel : available_plls) {
                    Loc pllloc = ctx->getBelLocation(bel);
                    int distance = std::abs(drvloc.x - pllloc.x) + std::abs(drvloc.y - pllloc.y);
                    if (distance < closest_distance) {
                        closest_pll = bel;
                        closest_distance = distance;
                    }
                }
                if (closest_pll == BelId())
                    log_error("failed to place PLL '%s'\n", ci->name.c_str(ctx));
                available_plls.erase(closest_pll);
                ci->attrs[id_BEL] = ctx->getBelName(closest_pll).str(ctx);
            }
        }
        // Place PLLs driven by logic, etc, randomly
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_EHXPLLL && !ci->attrs.count(id_BEL)) {
                if (available_plls.empty())
                    log_error("failed to place PLL '%s'\n", ci->name.c_str(ctx));
                BelId next_pll = *(available_plls.begin());
                available_plls.erase(next_pll);
                ci->attrs[id_BEL] = ctx->getBelName(next_pll).str(ctx);
            }
        }
    }

    // Check if two nets have identical constant drivers
    bool equal_constant(NetInfo *a, NetInfo *b)
    {
        if (a->driver.cell == nullptr || b->driver.cell == nullptr)
            return (a->driver.cell == nullptr && b->driver.cell == nullptr);
        if (a->driver.cell->type != id_GND && a->driver.cell->type != id_VCC)
            return false;
        return a->driver.cell->type == b->driver.cell->type;
    }

    struct EdgeClockInfo
    {
        CellInfo *buffer = nullptr;
        NetInfo *unbuf = nullptr;
        NetInfo *buf = nullptr;
    };

    std::map<std::pair<int, int>, EdgeClockInfo> eclks;
    std::map<NetInfo *, int> bridge_side_hint;

    void make_eclk(PortInfo &usr_port, CellInfo *usr_cell, BelId usr_bel, int bank)
    {
        NetInfo *ecknet = usr_port.net;
        if (ecknet == nullptr)
            log_error("Input '%s' of cell '%s' cannot be disconnected\n", usr_port.name.c_str(ctx),
                      usr_cell->name.c_str(ctx));
        int found_eclk = -1, free_eclk = -1;
        for (int i = 0; i < 2; i++) {
            if (eclks.count(std::make_pair(bank, i))) {
                if (eclks.at(std::make_pair(bank, i)).unbuf == ecknet) {
                    found_eclk = i;
                    break;
                }
            } else if (free_eclk == -1) {
                if (bridge_side_hint.count(ecknet) && bridge_side_hint.at(ecknet) != i)
                    continue;
                free_eclk = i;
            }
        }
        if (found_eclk == -1) {
            if (free_eclk == -1) {
                log_error("Unable to promote edge clock '%s' for bank %d. 2/2 edge clocks already used by '%s' and "
                          "'%s'.\n",
                          ecknet->name.c_str(ctx), bank, eclks.at(std::make_pair(bank, 0)).unbuf->name.c_str(ctx),
                          eclks.at(std::make_pair(bank, 1)).unbuf->name.c_str(ctx));
            } else {
                log_info("Promoted '%s' to bank %d ECLK%d.\n", ecknet->name.c_str(ctx), bank, free_eclk);
                auto &eclk = eclks[std::make_pair(bank, free_eclk)];
                eclk.unbuf = ecknet;
                IdString eckname = ctx->id(ecknet->name.str(ctx) + "$eclk" + std::to_string(bank) + "_" +
                                           std::to_string(free_eclk));

                NetInfo *promoted_ecknet = ctx->createNet(eckname);
                promoted_ecknet->attrs[id_ECP5_IS_GLOBAL] = 1; // Prevents router etc touching this special net
                eclk.buf = promoted_ecknet;

                // Insert TRELLIS_ECLKBUF to isolate edge clock from general routing
                std::unique_ptr<CellInfo> eclkbuf =
                        create_ecp5_cell(ctx, id_TRELLIS_ECLKBUF, eckname.str(ctx) + "$buffer");
                BelId target_bel;
                // Find the correct Bel for the ECLKBUF
                IdString eclkname = ctx->id("G_BANK" + std::to_string(bank) + "ECLK" + std::to_string(free_eclk));
                for (auto bel : ctx->getBels()) {
                    if (ctx->getBelType(bel) != id_TRELLIS_ECLKBUF)
                        continue;
                    if (ctx->get_wire_basename(ctx->getBelPinWire(bel, id_ECLKO)) != eclkname)
                        continue;
                    target_bel = bel;
                    break;
                }
                NPNR_ASSERT(target_bel != BelId());

                eclkbuf->attrs[id_BEL] = ctx->getBelName(target_bel).str(ctx);

                eclkbuf->connectPort(id_ECLKI, ecknet);
                eclkbuf->connectPort(id_ECLKO, eclk.buf);
                found_eclk = free_eclk;
                eclk.buffer = eclkbuf.get();
                new_cells.push_back(std::move(eclkbuf));
            }
        }

        auto &eclk = eclks[std::make_pair(bank, found_eclk)];
        usr_cell->disconnectPort(usr_port.name);
        usr_port.net = nullptr;
        usr_cell->connectPort(usr_port.name, eclk.buf);

        // Simple ECLK router
        WireId userWire = ctx->getBelPinWire(usr_bel, usr_port.name);
        IdString bnke_name = ctx->id("BNK_ECLK" + std::to_string(found_eclk));
        IdString global_name = ctx->id("G_BANK" + std::to_string(bank) + "ECLK" + std::to_string(found_eclk));

        std::queue<WireId> upstream;
        dict<WireId, PipId> backtrace;
        upstream.push(userWire);
        WireId next;
        while (true) {
            if (upstream.empty() || upstream.size() > 30000)
                log_error("failed to route bank %d ECLK%d to %s.%s\n", bank, found_eclk, ctx->nameOfBel(usr_bel),
                          usr_port.name.c_str(ctx));
            next = upstream.front();
            upstream.pop();
            if (ctx->debug)
                log_info("    visited %s\n", ctx->nameOfWire(next));
            IdString basename = ctx->get_wire_basename(next);
            if (basename == bnke_name || basename == global_name) {
                break;
            }
            if (ctx->checkWireAvail(next)) {
                for (auto pip : ctx->getPipsUphill(next)) {
                    WireId src = ctx->getPipSrcWire(pip);
                    backtrace[src] = pip;
                    upstream.push(src);
                }
            }
        }
        // Set all the pips we found along the way
        WireId cursor = next;
        while (true) {
            auto fnd = backtrace.find(cursor);
            if (fnd == backtrace.end())
                break;
            ctx->bindPip(fnd->second, eclk.buf, STRENGTH_LOCKED);
            cursor = ctx->getPipDstWire(fnd->second);
        }
    }

    void tie_zero(CellInfo *ci, IdString port)
    {

        if (!ci->ports.count(port)) {
            ci->ports[port].name = port;
            ci->ports[port].type = PORT_IN;
        }
        IdString name = ctx->id(ci->name.str(ctx) + "$zero$" + port.str(ctx));

        auto zero_cell = std::make_unique<CellInfo>(ctx, name, id_GND);
        NetInfo *zero_net = ctx->createNet(name);
        zero_cell->addOutput(id_GND);
        zero_cell->connectPort(id_GND, zero_net);
        ci->connectPort(port, zero_net);
        new_cells.push_back(std::move(zero_cell));
    }

    dict<IdString, std::pair<bool, int>> dqsbuf_dqsg;
    // Pack DQSBUFs
    void pack_dqsbuf()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_DQSBUFM) {
                CellInfo *pio = net_driven_by(ctx, ci->ports.at(id_DQSI).net, is_trellis_io, id_O);
                if (pio == nullptr || ci->ports.at(id_DQSI).net->users.entries() > 1)
                    log_error("DQSBUFM '%s' DQSI input must be connected only to a top level input\n",
                              ci->name.c_str(ctx));
                if (!pio->attrs.count(id_BEL))
                    log_error("DQSBUFM can only be used with a pin-constrained PIO connected to its DQSI input"
                              "(while processing '%s').\n",
                              ci->name.c_str(ctx));
                BelId pio_bel = ctx->getBelByNameStr(pio->attrs.at(id_BEL).as_string());
                NPNR_ASSERT(pio_bel != BelId());
                Loc pio_loc = ctx->getBelLocation(pio_bel);
                if (pio_loc.z != 0)
                    log_error("PIO '%s' does not appear to be a DQS site (expecting an 'A' pin).\n",
                              ctx->nameOfBel(pio_bel));
                pio_loc.z = 8;
                BelId dqsbuf = ctx->getBelByLocation(pio_loc);
                if (dqsbuf == BelId() || ctx->getBelType(dqsbuf) != id_DQSBUFM)
                    log_error("PIO '%s' does not appear to be a DQS site (didn't find a DQSBUFM).\n",
                              ctx->nameOfBel(pio_bel));
                ci->attrs[id_BEL] = ctx->getBelName(dqsbuf).str(ctx);
                bool got_dqsg =
                        ctx->get_pio_dqs_group(pio_bel, dqsbuf_dqsg[ci->name].first, dqsbuf_dqsg[ci->name].second);
                NPNR_ASSERT(got_dqsg);
                log_info("Constrained DQSBUFM '%s' to %cDQS%d\n", ci->name.c_str(ctx),
                         dqsbuf_dqsg[ci->name].first ? 'R' : 'L', dqsbuf_dqsg[ci->name].second);

                // Set all special ports, if used as 'globals' that the router won't touch
                for (auto port : {id_DQSR90, id_RDPNTR0, id_RDPNTR1, id_RDPNTR2, id_WRPNTR0, id_WRPNTR1, id_WRPNTR2,
                                  id_DQSW270, id_DQSW}) {
                    if (!ci->ports.count(port))
                        continue;
                    NetInfo *pn = ci->ports.at(port).net;
                    if (pn == nullptr)
                        continue;
                    for (auto &usr : pn->users) {
                        if (usr.port != port || (usr.cell->type != id_ODDRX2DQA && usr.cell->type != id_ODDRX2DQSB &&
                                                 usr.cell->type != id_TSHX2DQSA && usr.cell->type != id_IDDRX2DQA &&
                                                 usr.cell->type != id_TSHX2DQA && usr.cell->type != id_IOLOGIC))
                            log_error("Port '%s' of DQSBUFM '%s' cannot drive port '%s' of cell '%s'.\n",
                                      port.c_str(ctx), ci->name.c_str(ctx), usr.port.c_str(ctx),
                                      usr.cell->name.c_str(ctx));
                    }
                    pn->attrs[id_ECP5_IS_GLOBAL] = 1;
                }

                for (auto zport :
                     {id_RDMOVE, id_RDDIRECTION, id_WRMOVE, id_WRDIRECTION, id_READ0, id_READ1, id_READCLKSEL0,
                      id_READCLKSEL1, id_READCLKSEL2, id_DYNDELAY0, id_DYNDELAY1, id_DYNDELAY2, id_DYNDELAY3,
                      id_DYNDELAY4, id_DYNDELAY5, id_DYNDELAY6, id_DYNDELAY7}) {
                    if (net_or_nullptr(ci, zport) == nullptr)
                        tie_zero(ci, zport);
                }
            }
        }
    }

    int lookup_delay(const std::string &del_mode)
    {
        if (del_mode == "USER_DEFINED")
            return 0;
        else if (del_mode == "DQS_ALIGNED_X2")
            return 6;
        else if (del_mode == "DQS_CMD_CLK")
            return 9;
        else if (del_mode == "ECLK_ALIGNED")
            return 21;
        else if (del_mode == "ECLK_CENTERED")
            return 11;
        else if (del_mode == "ECLKBRIDGE_ALIGNED")
            return 39;
        else if (del_mode == "ECLKBRIDGE_CENTERED")
            return 29;
        else if (del_mode == "SCLK_ALIGNED")
            return 50;
        else if (del_mode == "SCLK_CENTERED")
            return 39;
        else if (del_mode == "SCLK_ZEROHOLD")
            return 59;
        else
            log_error("Unsupported DEL_MODE '%s'\n", del_mode.c_str());
    }

    // Pack IOLOGIC
    void pack_iologic()
    {
        dict<IdString, CellInfo *> pio_iologic;

        auto set_iologic_sclk = [&](CellInfo *iol, CellInfo *prim, IdString port, bool input, bool disconnect = true) {
            NetInfo *sclk = nullptr;
            if (prim->ports.count(port))
                sclk = prim->ports[port].net;
            if (sclk == nullptr) {
                iol->params[input ? id_CLKIMUX : id_CLKOMUX] = std::string("0");
            } else {
                iol->params[input ? id_CLKIMUX : id_CLKOMUX] = std::string("CLK");
                if (iol->ports[id_CLK].net != nullptr) {
                    if (iol->ports[id_CLK].net != sclk && !equal_constant(iol->ports[id_CLK].net, sclk))
                        log_error("IOLOGIC '%s' has conflicting clocks '%s' and '%s'\n", iol->name.c_str(ctx),
                                  iol->ports[id_CLK].net->name.c_str(ctx), sclk->name.c_str(ctx));
                } else {
                    iol->connectPort(id_CLK, sclk);
                }
            }
            if (prim->ports.count(port) && disconnect)
                prim->disconnectPort(port);
        };

        auto set_iologic_eclk = [&](CellInfo *iol, CellInfo *prim, IdString port) {
            NetInfo *eclk = nullptr;
            if (prim->ports.count(port))
                eclk = prim->ports[port].net;
            if (eclk == nullptr)
                log_error("%s '%s' cannot have disconnected ECLK", prim->type.c_str(ctx), prim->name.c_str(ctx));

            if (iol->ports[id_ECLK].net != nullptr) {
                if (iol->ports[id_ECLK].net != eclk)
                    log_error("IOLOGIC '%s' has conflicting ECLKs '%s' and '%s'\n", iol->name.c_str(ctx),
                              iol->ports[id_ECLK].net->name.c_str(ctx), eclk->name.c_str(ctx));
            } else {
                iol->connectPort(id_ECLK, eclk);
            }
            if (prim->ports.count(port))
                prim->disconnectPort(port);
        };

        auto set_iologic_lsr = [&](CellInfo *iol, CellInfo *prim, IdString port, bool input, bool disconnect = true) {
            NetInfo *lsr = nullptr;
            if (prim->ports.count(port))
                lsr = prim->ports[port].net;
            if (lsr == nullptr) {
                iol->params[input ? id_LSRIMUX : id_LSROMUX] = std::string("0");
            } else {
                iol->params[input ? id_LSRIMUX : id_LSROMUX] = std::string("LSRMUX");
                if (iol->ports[id_LSR].net != nullptr && !equal_constant(iol->ports[id_LSR].net, lsr)) {
                    if (iol->ports[id_LSR].net != lsr)
                        log_error("IOLOGIC '%s' has conflicting LSR signals '%s' and '%s'\n", iol->name.c_str(ctx),
                                  iol->ports[id_LSR].net->name.c_str(ctx), lsr->name.c_str(ctx));
                } else if (iol->ports[id_LSR].net == nullptr) {
                    iol->connectPort(id_LSR, lsr);
                }
            }
            if (prim->ports.count(port) && disconnect)
                prim->disconnectPort(port);
        };

        bool warned_oddrx_iddrx = false;

        auto set_iologic_mode = [&](CellInfo *iol, std::string mode) {
            auto &curr_mode = iol->params[id_MODE].str;
            if (curr_mode != "NONE" && mode == "IREG_OREG")
                return;
            if ((curr_mode == "IDDRXN" && mode == "ODDRXN") || (curr_mode == "ODDRXN" && mode == "IDDRXN")) {
                if (!warned_oddrx_iddrx) {
                    warned_oddrx_iddrx = true;
                    log_warning("Use of IDDRXN and ODDRXN primitives on the same pin is unofficial and unsupported!\n");
                }
                curr_mode = "ODDRXN";
                return;
            }
            if (curr_mode != "NONE" && curr_mode != "IREG_OREG" && curr_mode != mode)
                log_error("IOLOGIC '%s' has conflicting modes '%s' and '%s'\n", iol->name.c_str(ctx), curr_mode.c_str(),
                          mode.c_str());
            if (iol->type == id_SIOLOGIC && mode != "IREG_OREG" && mode != "IDDRX1_ODDRX1" && mode != "NONE")
                log_error("IOLOGIC '%s' is set to mode '%s', but this is only supported for left and right IO\n",
                          iol->name.c_str(ctx), mode.c_str());
            curr_mode = mode;
        };

        auto get_pio_bel = [&](CellInfo *pio, CellInfo *curr) {
            if (!pio->attrs.count(id_BEL))
                log_error("IOLOGIC functionality (DDR, DELAY, DQS, etc) can only be used with pin-constrained PIO "
                          "(while processing '%s').\n",
                          curr->name.c_str(ctx));
            BelId bel = ctx->getBelByNameStr(pio->attrs.at(id_BEL).as_string());
            NPNR_ASSERT(bel != BelId());
            return bel;
        };

        auto create_pio_iologic = [&](CellInfo *pio, CellInfo *curr) {
            BelId bel = get_pio_bel(pio, curr);
            log_info("IOLOGIC component %s connected to PIO Bel %s\n", curr->name.c_str(ctx), ctx->nameOfBel(bel));
            Loc loc = ctx->getBelLocation(bel);
            bool s = false;
            if (loc.y == 0 || loc.y == (ctx->chip_info->height - 1))
                s = true;
            std::unique_ptr<CellInfo> iol =
                    create_ecp5_cell(ctx, s ? id_SIOLOGIC : id_IOLOGIC, pio->name.str(ctx) + "$IOL");

            loc.z += s ? 2 : 4;
            iol->attrs[id_BEL] = ctx->getBelName(ctx->getBelByLocation(loc)).str(ctx);

            CellInfo *iol_ptr = iol.get();
            pio_iologic[pio->name] = iol_ptr;
            new_cells.push_back(std::move(iol));
            return iol_ptr;
        };

        auto process_dqs_port = [&](CellInfo *prim, CellInfo *pio, CellInfo *iol, IdString port) {
            NetInfo *sig = nullptr;
            if (prim->ports.count(port))
                sig = prim->ports[port].net;
            if (sig == nullptr || sig->driver.cell == nullptr)
                log_error("Port %s of cell '%s' cannot be disconnected, it must be driven by a DQSBUFM\n",
                          port.c_str(ctx), prim->name.c_str(ctx));
            if (iol->ports.at(port).net != nullptr) {
                if (iol->ports.at(port).net != sig) {
                    log_error("IOLOGIC '%s' has conflicting %s signals '%s' and '%s'\n", iol->name.c_str(ctx),
                              port.c_str(ctx), iol->ports[port].net->name.c_str(ctx), sig->name.c_str(ctx));
                }
                prim->disconnectPort(port);
            } else {
                bool dqsr;
                int dqsgroup;
                bool has_dqs = ctx->get_pio_dqs_group(get_pio_bel(pio, prim), dqsr, dqsgroup);
                if (!has_dqs)
                    log_error("Primitive '%s' cannot be connected to top level port '%s' as the associated pin is not "
                              "in any DQS group",
                              prim->name.c_str(ctx), pio->name.c_str(ctx));
                if (sig->driver.cell->type != id_DQSBUFM || sig->driver.port != port)
                    log_error("Port %s of cell '%s' must be driven by port %s of a DQSBUFM", port.c_str(ctx),
                              prim->name.c_str(ctx), port.c_str(ctx));
                auto &driver_group = dqsbuf_dqsg.at(sig->driver.cell->name);
                if (driver_group.first != dqsr || driver_group.second != dqsgroup)
                    log_error("DQS group mismatch, port %s of '%s' in group %cDQ%d is driven by DQSBUFM '%s' in group "
                              "%cDQ%d\n",
                              port.c_str(ctx), prim->name.c_str(ctx), dqsr ? 'R' : 'L', dqsgroup,
                              sig->driver.cell->name.c_str(ctx), driver_group.first ? 'R' : 'L', driver_group.second);
                prim->movePortTo(port, iol, port);
            }
        };

        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_DELAYF || ci->type == id_DELAYG) {
                CellInfo *i_pio = net_driven_by(ctx, ci->ports.at(id_A).net, is_trellis_io, id_O);
                CellInfo *o_pio = net_only_drives(ctx, ci->ports.at(id_Z).net, is_trellis_io, id_I, true);
                CellInfo *iol = nullptr;
                if (i_pio != nullptr && ci->ports.at(id_A).net->users.entries() == 1) {
                    iol = create_pio_iologic(i_pio, ci);
                    set_iologic_mode(iol, "IREG_OREG");
                    bool drives_iologic = false;
                    for (auto user : ci->ports.at(id_Z).net->users)
                        if (is_iologic_input_cell(ctx, user.cell) &&
                            (user.port == id_D || (user.cell->type == id_TRELLIS_FF && user.port == id_DI)))
                            drives_iologic = true;
                    if (drives_iologic) {
                        // Reconnect to PIO which the packer expects later on
                        NetInfo *input_net = ci->ports.at(id_A).net, *dly_net = ci->ports.at(id_Z).net;
                        i_pio->disconnectPort(id_O);
                        i_pio->ports.at(id_O).net = nullptr;
                        ci->disconnectPort(id_A);
                        ci->ports.at(id_A).net = nullptr;
                        ci->disconnectPort(id_Z);
                        ci->ports.at(id_Z).net = nullptr;
                        i_pio->connectPort(id_O, dly_net);
                        iol->connectPort(id_INDD, input_net);
                        iol->connectPort(id_DI, input_net);
                    } else {
                        ci->movePortTo(id_A, iol, id_PADDI);
                        ci->movePortTo(id_Z, iol, id_INDD);
                    }
                    packed_cells.insert(cell.first);
                } else if (o_pio != nullptr) {
                    iol = create_pio_iologic(o_pio, ci);
                    iol->params[ctx->id("DELAY.OUTDEL")] = std::string("ENABLED");
                    bool driven_by_iol = false;
                    NetInfo *input_net = ci->ports.at(id_A).net, *dly_net = ci->ports.at(id_Z).net;
                    if (input_net->driver.cell != nullptr && is_iologic_output_cell(ctx, input_net->driver.cell) &&
                        input_net->driver.port == id_Q)
                        driven_by_iol = true;
                    if (driven_by_iol) {
                        o_pio->disconnectPort(id_I);
                        o_pio->ports.at(id_I).net = nullptr;
                        ci->disconnectPort(id_A);
                        ci->ports.at(id_A).net = nullptr;
                        ci->disconnectPort(id_Z);
                        ci->ports.at(id_Z).net = nullptr;
                        o_pio->connectPort(id_I, input_net);
                        ctx->nets.erase(dly_net->name);
                    } else {
                        ci->movePortTo(id_A, iol, id_TXDATA0);
                        ci->movePortTo(id_Z, iol, id_IOLDO);
                        if (!o_pio->ports.count(id_IOLDO)) {
                            o_pio->ports[id_IOLDO].name = id_IOLDO;
                            o_pio->ports[id_IOLDO].type = PORT_IN;
                        }
                        o_pio->movePortTo(id_I, o_pio, id_IOLDO);
                    }
                    packed_cells.insert(cell.first);
                } else {
                    log_error("%s '%s' must be connected directly to top level input or output\n", ci->type.c_str(ctx),
                              ci->name.c_str(ctx));
                }
                iol->params[ctx->id("DELAY.DEL_VALUE")] =
                        lookup_delay(str_or_default(ci->params, id_DEL_MODE, "USER_DEFINED"));
                if (ci->params.count(id_DEL_VALUE) &&
                    (!ci->params.at(id_DEL_VALUE).is_string ||
                     std::string(ci->params.at(id_DEL_VALUE).as_string()).substr(0, 5) != "DELAY"))
                    iol->params[ctx->id("DELAY.DEL_VALUE")] = ci->params.at(id_DEL_VALUE);
                if (ci->ports.count(id_LOADN))
                    ci->movePortTo(id_LOADN, iol, id_LOADN);
                else
                    tie_zero(iol, id_LOADN);
                if (ci->ports.count(id_MOVE))
                    ci->movePortTo(id_MOVE, iol, id_MOVE);
                else
                    tie_zero(iol, id_MOVE);
                if (ci->ports.count(id_DIRECTION))
                    ci->movePortTo(id_DIRECTION, iol, id_DIRECTION);
                else
                    tie_zero(iol, id_DIRECTION);
                if (ci->ports.count(id_CFLAG))
                    ci->movePortTo(id_CFLAG, iol, id_CFLAG);
            }
        }

        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_IDDRX1F) {
                CellInfo *pio = net_driven_by(ctx, ci->ports.at(id_D).net, is_trellis_io, id_O);
                if (pio == nullptr || ci->ports.at(id_D).net->users.entries() > 1)
                    log_error("IDDRX1F '%s' D input must be connected only to a top level input\n",
                              ci->name.c_str(ctx));
                CellInfo *iol;
                if (pio_iologic.count(pio->name))
                    iol = pio_iologic.at(pio->name);
                else
                    iol = create_pio_iologic(pio, ci);
                set_iologic_mode(iol, "IDDRX1_ODDRX1");
                ci->movePortTo(id_D, iol, id_PADDI);
                set_iologic_sclk(iol, ci, id_SCLK, true);
                set_iologic_lsr(iol, ci, id_RST, true);
                ci->movePortTo(id_Q0, iol, id_RXDATA0);
                ci->movePortTo(id_Q1, iol, id_RXDATA1);
                iol->params[id_GSR] = str_or_default(ci->params, id_GSR, "DISABLED");
                packed_cells.insert(cell.first);
            } else if (ci->type == id_ODDRX1F) {
                CellInfo *pio = net_only_drives(ctx, ci->ports.at(id_Q).net, is_trellis_io, id_I, true);
                if (pio == nullptr)
                    log_error("ODDRX1F '%s' Q output must be connected only to a top level output\n",
                              ci->name.c_str(ctx));
                CellInfo *iol;
                if (pio_iologic.count(pio->name))
                    iol = pio_iologic.at(pio->name);
                else
                    iol = create_pio_iologic(pio, ci);
                set_iologic_mode(iol, "IDDRX1_ODDRX1");
                ci->movePortTo(id_Q, iol, id_IOLDO);
                if (!pio->ports.count(id_IOLDO)) {
                    pio->ports[id_IOLDO].name = id_IOLDO;
                    pio->ports[id_IOLDO].type = PORT_IN;
                }
                pio->movePortTo(id_I, pio, id_IOLDO);
                pio->params[id_DATAMUX_ODDR] = std::string("IOLDO");
                set_iologic_sclk(iol, ci, id_SCLK, false);
                set_iologic_lsr(iol, ci, id_RST, false);
                ci->movePortTo(id_D0, iol, id_TXDATA0);
                ci->movePortTo(id_D1, iol, id_TXDATA1);
                iol->params[id_GSR] = str_or_default(ci->params, id_GSR, "DISABLED");
                packed_cells.insert(cell.first);
            } else if (ci->type == id_ODDRX2F || ci->type == id_ODDR71B) {
                CellInfo *pio = net_only_drives(ctx, ci->ports.at(id_Q).net, is_trellis_io, id_I, true);
                if (pio == nullptr)
                    log_error("%s '%s' Q output must be connected only to a top level output\n", ci->type.c_str(ctx),
                              ci->name.c_str(ctx));
                CellInfo *iol;
                if (pio_iologic.count(pio->name))
                    iol = pio_iologic.at(pio->name);
                else
                    iol = create_pio_iologic(pio, ci);
                set_iologic_mode(iol, "ODDRXN");
                ci->movePortTo(id_Q, iol, id_IOLDO);
                if (!pio->ports.count(id_IOLDO)) {
                    pio->ports[id_IOLDO].name = id_IOLDO;
                    pio->ports[id_IOLDO].type = PORT_IN;
                }
                pio->movePortTo(id_I, pio, id_IOLDO);
                set_iologic_sclk(iol, ci, id_SCLK, false, false);
                set_iologic_sclk(iol, ci, id_SCLK, true);
                set_iologic_eclk(iol, ci, id_ECLK);
                set_iologic_lsr(iol, ci, id_RST, false, false);
                set_iologic_lsr(iol, ci, id_RST, true);
                ci->movePortTo(id_D0, iol, id_TXDATA0);
                ci->movePortTo(id_D1, iol, id_TXDATA1);
                ci->movePortTo(id_D2, iol, id_TXDATA2);
                ci->movePortTo(id_D3, iol, id_TXDATA3);
                if (ci->type == id_ODDR71B) {
                    Loc loc = ctx->getBelLocation(ctx->getBelByNameStr(pio->attrs.at(id_BEL).as_string()));
                    if (loc.z % 2 == 1)
                        log_error("ODDR71B '%s' can only be used at 'A' or 'C' locations\n", ci->name.c_str(ctx));
                    ci->movePortTo(id_D4, iol, id_TXDATA4);
                    ci->movePortTo(id_D5, iol, id_TXDATA5);
                    ci->movePortTo(id_D6, iol, id_TXDATA6);
                    iol->params[ctx->id("ODDRXN.MODE")] = std::string("ODDR71");
                } else {
                    iol->params[ctx->id("ODDRXN.MODE")] = std::string("ODDRX2");
                }
                iol->params[id_GSR] = str_or_default(ci->params, id_GSR, "DISABLED");
                pio->params[id_DATAMUX_ODDR] = std::string("IOLDO");
                packed_cells.insert(cell.first);
            } else if (ci->type == id_IDDRX2F || ci->type == id_IDDR71B) {
                CellInfo *pio = net_driven_by(ctx, ci->ports.at(id_D).net, is_trellis_io, id_O);
                if (pio == nullptr || ci->ports.at(id_D).net->users.entries() > 1)
                    log_error("%s '%s' D input must be connected only to a top level input\n", ci->type.c_str(ctx),
                              ci->name.c_str(ctx));
                CellInfo *iol;
                if (pio_iologic.count(pio->name))
                    iol = pio_iologic.at(pio->name);
                else
                    iol = create_pio_iologic(pio, ci);
                set_iologic_mode(iol, "IDDRXN");
                ci->movePortTo(id_D, iol, id_PADDI);
                set_iologic_sclk(iol, ci, id_SCLK, true);
                set_iologic_eclk(iol, ci, id_ECLK);
                set_iologic_lsr(iol, ci, id_RST, true);
                ci->movePortTo(id_Q0, iol, id_RXDATA0);
                ci->movePortTo(id_Q1, iol, id_RXDATA1);
                ci->movePortTo(id_Q2, iol, id_RXDATA2);
                ci->movePortTo(id_Q3, iol, id_RXDATA3);
                if (ci->type == id_IDDR71B) {
                    Loc loc = ctx->getBelLocation(ctx->getBelByNameStr(pio->attrs.at(id_BEL).as_string()));
                    if (loc.z % 2 == 1)
                        log_error("IDDR71B '%s' can only be used at 'A' or 'C' locations\n", ci->name.c_str(ctx));
                    ci->movePortTo(id_Q4, iol, id_RXDATA4);
                    ci->movePortTo(id_Q5, iol, id_RXDATA5);
                    ci->movePortTo(id_Q6, iol, id_RXDATA6);
                    ci->movePortTo(id_ALIGNWD, iol, id_SLIP);
                    iol->params[ctx->id("IDDRXN.MODE")] = std::string("IDDR71");
                } else {
                    iol->params[ctx->id("IDDRXN.MODE")] = std::string("IDDRX2");
                }
                iol->params[id_GSR] = str_or_default(ci->params, id_GSR, "DISABLED");
                packed_cells.insert(cell.first);
            } else if (ci->type == id_OSHX2A) {
                CellInfo *pio = net_only_drives(ctx, ci->ports.at(id_Q).net, is_trellis_io, id_I, true);
                if (pio == nullptr)
                    log_error("OSHX2A '%s' Q output must be connected only to a top level output\n",
                              ci->name.c_str(ctx));
                CellInfo *iol;
                if (pio_iologic.count(pio->name))
                    iol = pio_iologic.at(pio->name);
                else
                    iol = create_pio_iologic(pio, ci);
                set_iologic_mode(iol, "MIDDRX_MODDRX");
                ci->movePortTo(id_Q, iol, id_IOLDO);
                if (!pio->ports.count(id_IOLDO)) {
                    pio->ports[id_IOLDO].name = id_IOLDO;
                    pio->ports[id_IOLDO].type = PORT_IN;
                }
                pio->movePortTo(id_I, pio, id_IOLDO);
                set_iologic_sclk(iol, ci, id_SCLK, false);
                set_iologic_eclk(iol, ci, id_ECLK);
                set_iologic_lsr(iol, ci, id_RST, false, false);
                set_iologic_lsr(iol, ci, id_RST, true);
                ci->movePortTo(id_D0, iol, id_TXDATA0);
                ci->movePortTo(id_D1, iol, id_TXDATA2);
                iol->params[id_GSR] = str_or_default(ci->params, id_GSR, "DISABLED");
                iol->params[ctx->id("MODDRX.MODE")] = std::string("MOSHX2");
                pio->params[id_DATAMUX_MDDR] = std::string("IOLDO");
                packed_cells.insert(cell.first);
            } else if (ci->type == id_ODDRX2DQA || ci->type == id_ODDRX2DQSB) {
                CellInfo *pio = net_only_drives(ctx, ci->ports.at(id_Q).net, is_trellis_io, id_I, true);
                if (pio == nullptr)
                    log_error("%s '%s' Q output must be connected only to a top level output\n", ci->type.c_str(ctx),
                              ci->name.c_str(ctx));
                CellInfo *iol;
                if (pio_iologic.count(pio->name))
                    iol = pio_iologic.at(pio->name);
                else
                    iol = create_pio_iologic(pio, ci);
                set_iologic_mode(iol, "MIDDRX_MODDRX");
                ci->movePortTo(id_Q, iol, id_IOLDO);
                if (!pio->ports.count(id_IOLDO)) {
                    pio->ports[id_IOLDO].name = id_IOLDO;
                    pio->ports[id_IOLDO].type = PORT_IN;
                }
                pio->movePortTo(id_I, pio, id_IOLDO);
                set_iologic_sclk(iol, ci, id_SCLK, false);
                set_iologic_eclk(iol, ci, id_ECLK);
                set_iologic_lsr(iol, ci, id_RST, false, false);
                set_iologic_lsr(iol, ci, id_RST, true);
                ci->movePortTo(id_D0, iol, id_TXDATA0);
                ci->movePortTo(id_D1, iol, id_TXDATA1);
                ci->movePortTo(id_D2, iol, id_TXDATA2);
                ci->movePortTo(id_D3, iol, id_TXDATA3);
                iol->params[id_GSR] = str_or_default(ci->params, id_GSR, "DISABLED");
                iol->params[ctx->id("MODDRX.MODE")] = std::string("MODDRX2");
                iol->params[ctx->id("MIDDRX_MODDRX.WRCLKMUX")] =
                        std::string(ci->type == id_ODDRX2DQSB ? "DQSW" : "DQSW270");
                process_dqs_port(ci, pio, iol, ci->type == id_ODDRX2DQSB ? id_DQSW : id_DQSW270);
                pio->params[id_DATAMUX_MDDR] = std::string("IOLDO");
                packed_cells.insert(cell.first);
            } else if (ci->type == id_IDDRX2DQA) {
                CellInfo *pio = net_driven_by(ctx, ci->ports.at(id_D).net, is_trellis_io, id_O);
                if (pio == nullptr || ci->ports.at(id_D).net->users.entries() > 1)
                    log_error("IDDRX2DQA '%s' D input must be connected only to a top level input\n",
                              ci->name.c_str(ctx));
                CellInfo *iol;
                if (pio_iologic.count(pio->name))
                    iol = pio_iologic.at(pio->name);
                else
                    iol = create_pio_iologic(pio, ci);
                set_iologic_mode(iol, "MIDDRX_MODDRX");
                ci->movePortTo(id_D, iol, id_PADDI);
                set_iologic_sclk(iol, ci, id_SCLK, true);
                set_iologic_eclk(iol, ci, id_ECLK);
                set_iologic_lsr(iol, ci, id_RST, true);
                ci->movePortTo(id_Q0, iol, id_RXDATA0);
                ci->movePortTo(id_Q1, iol, id_RXDATA1);
                ci->movePortTo(id_Q2, iol, id_RXDATA2);
                ci->movePortTo(id_Q3, iol, id_RXDATA3);
                ci->movePortTo(id_QWL, iol, id_INFF);
                iol->params[id_GSR] = str_or_default(ci->params, id_GSR, "DISABLED");
                iol->params[ctx->id("MIDDRX.MODE")] = std::string("MIDDRX2");
                process_dqs_port(ci, pio, iol, id_DQSR90);
                process_dqs_port(ci, pio, iol, id_RDPNTR2);
                process_dqs_port(ci, pio, iol, id_RDPNTR1);
                process_dqs_port(ci, pio, iol, id_RDPNTR0);
                process_dqs_port(ci, pio, iol, id_WRPNTR2);
                process_dqs_port(ci, pio, iol, id_WRPNTR1);
                process_dqs_port(ci, pio, iol, id_WRPNTR0);
                packed_cells.insert(cell.first);
            } else if (ci->type == id_TSHX2DQA || ci->type == id_TSHX2DQSA) {
                CellInfo *pio = net_only_drives(ctx, ci->ports.at(id_Q).net, is_trellis_io, id_T, true);
                if (pio == nullptr)
                    log_error("%s '%s' Q output must be connected only to a top level tristate\n", ci->type.c_str(ctx),
                              ci->name.c_str(ctx));
                CellInfo *iol;
                if (pio_iologic.count(pio->name))
                    iol = pio_iologic.at(pio->name);
                else
                    iol = create_pio_iologic(pio, ci);
                set_iologic_mode(iol, "MIDDRX_MODDRX");
                ci->movePortTo(id_Q, iol, id_IOLTO);
                if (!pio->ports.count(id_IOLTO)) {
                    pio->ports[id_IOLTO].name = id_IOLTO;
                    pio->ports[id_IOLTO].type = PORT_IN;
                }
                pio->movePortTo(id_T, pio, id_IOLTO);
                set_iologic_sclk(iol, ci, id_SCLK, false);
                set_iologic_eclk(iol, ci, id_ECLK);
                set_iologic_lsr(iol, ci, id_RST, false);
                ci->movePortTo(id_T0, iol, id_TSDATA0);
                ci->movePortTo(id_T1, iol, id_TSDATA1);
                process_dqs_port(ci, pio, iol, ci->type == id_TSHX2DQSA ? id_DQSW : id_DQSW270);
                iol->params[id_GSR] = str_or_default(ci->params, id_GSR, "DISABLED");
                iol->params[ctx->id("MTDDRX.MODE")] = std::string("MTSHX2");
                iol->params[ctx->id("MTDDRX.REGSET")] = std::string("SET");
                iol->params[ctx->id("MTDDRX.DQSW_INVERT")] =
                        std::string(ci->type == id_TSHX2DQSA ? "ENABLED" : "DISABLED");
                iol->params[ctx->id("MIDDRX_MODDRX.WRCLKMUX")] =
                        std::string(ci->type == id_TSHX2DQSA ? "DQSW" : "DQSW270");
                iol->params[id_IOLTOMUX] = std::string("TDDR");
                packed_cells.insert(cell.first);
            } else if (ci->type == id_TRELLIS_FF && bool_or_default(ci->attrs, id_syn_useioff)) {
                // Pack IO flipflop into IOLOGIC
                std::string mode = str_or_default(ci->attrs, id_ioff_dir, "");
                if (mode != "output") {
                    // See if it can be packed as an input ff
                    NetInfo *d = ci->getPort(id_DI);
                    CellInfo *pio = net_driven_by(ctx, d, is_trellis_io, id_O);
                    if (pio != nullptr && d->users.entries() == 1) {
                        // Input FF
                        CellInfo *iol;
                        if (pio_iologic.count(pio->name))
                            iol = pio_iologic.at(pio->name);
                        else
                            iol = create_pio_iologic(pio, ci);
                        set_iologic_mode(iol, "IREG_OREG");
                        set_iologic_sclk(iol, ci, id_CLK, true);
                        set_iologic_lsr(iol, ci, id_LSR, true);
                        // Handle CLK and CE muxes
                        if (str_or_default(ci->params, id_CLKMUX) == "INV")
                            iol->params[id_CLKIMUX] = std::string("INV");
                        if (str_or_default(ci->params, id_CEMUX, "CE") == "CE") {
                            iol->params[id_CEIMUX] = std::string("CEMUX");
                            iol->params[id_CEMUX] = std::string("CE");
                            if (ci->getPort(id_CE) == nullptr)
                                ci->movePortTo(id_CE, iol, id_CE);
                            else
                                ci->disconnectPort(id_CE);
                        } else {
                            iol->params[id_CEIMUX] = std::string("1");
                        }
                        // Set IOLOGIC params from FF params
                        iol->params[ctx->id("FF.INREGMODE")] = std::string("FF");
                        iol->params[ctx->id("FF.REGSET")] = str_or_default(ci->params, id_REGSET, "RESET");
                        iol->params[id_SRMODE] = str_or_default(ci->params, id_SRMODE, "ASYNC");
                        iol->params[id_GSR] = str_or_default(ci->params, id_GSR, "DISABLED");
                        ci->movePortTo(id_DI, iol, id_PADDI);
                        ci->movePortTo(id_Q, iol, id_INFF);
                        packed_cells.insert(cell.first);
                        continue;
                    }
                }
                if (mode != "input") {
                    CellInfo *pio_t = net_only_drives(ctx, ci->ports.at(id_Q).net, is_trellis_io, id_T, true);
                    CellInfo *pio_i = net_only_drives(ctx, ci->ports.at(id_Q).net, is_trellis_io, id_I, true);
                    if (pio_t != nullptr || pio_i != nullptr) {
                        // Output or tristate FF
                        bool tri = (pio_t != nullptr);
                        CellInfo *pio = tri ? pio_t : pio_i;
                        CellInfo *iol;
                        if (pio_iologic.count(pio->name))
                            iol = pio_iologic.at(pio->name);
                        else
                            iol = create_pio_iologic(pio, ci);
                        set_iologic_mode(iol, "IREG_OREG");
                        // Connection between FF and PIO
                        ci->movePortTo(id_Q, iol, tri ? id_IOLTO : id_IOLDO);
                        if (tri) {
                            if (!pio->ports.count(id_IOLTO)) {
                                pio->ports[id_IOLTO].name = id_IOLTO;
                                pio->ports[id_IOLTO].type = PORT_IN;
                            }
                            pio->params[id_TRIMUX_TSREG] = std::string("IOLTO");
                            pio->movePortTo(id_T, pio, id_IOLTO);
                        } else {
                            if (!pio->ports.count(id_IOLDO)) {
                                pio->ports[id_IOLDO].name = id_IOLDO;
                                pio->ports[id_IOLDO].type = PORT_IN;
                            }
                            pio->params[id_DATAMUX_OREG] = std::string("IOLDO");
                            pio->movePortTo(id_I, pio, id_IOLDO);
                        }

                        set_iologic_sclk(iol, ci, id_CLK, false);
                        set_iologic_lsr(iol, ci, id_LSR, false);

                        // Handle CLK and CE muxes
                        if (str_or_default(ci->params, id_CLKMUX) == "INV")
                            iol->params[id_CLKOMUX] = std::string("INV");
                        if (str_or_default(ci->params, id_CEMUX, "CE") == "CE") {
                            iol->params[id_CEOMUX] = std::string("CEMUX");
                            iol->params[id_CEMUX] = std::string("CE");
                            if (ci->getPort(id_CE) == nullptr)
                                ci->movePortTo(id_CE, iol, id_CE);
                            else
                                ci->disconnectPort(id_CE);
                        } else {
                            iol->params[id_CEOMUX] = std::string("1");
                        }
                        // FF params
                        iol->params[ctx->id(tri ? "TSREG.OUTREGMODE" : "OUTREG.OUTREGMODE")] = std::string("FF");
                        iol->params[ctx->id(tri ? "TSREG.REGSET" : "OUTREG.REGSET")] =
                                str_or_default(ci->params, id_REGSET, "RESET");
                        iol->params[id_SRMODE] = str_or_default(ci->params, id_SRMODE, "ASYNC");
                        // Data input
                        ci->movePortTo(id_DI, iol, tri ? id_TSDATA0 : id_TXDATA0);
                        iol->params[id_GSR] = str_or_default(ci->params, id_GSR, "DISABLED");
                        packed_cells.insert(cell.first);
                        continue;
                    }
                }
                log_error("Failed to pack flipflop '%s' with 'syn_useioff' set into IOLOGIC.\n", ci->name.c_str(ctx));
            }
        }
        flush_cells();
        // Constrain ECLK-related cells
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_ECLKBRIDGECS) {
                Loc loc;
                NetInfo *i0 = ci->getPort(id_CLK0), *i1 = ci->getPort(id_CLK1), *o = ci->getPort(id_ECSOUT);
                for (NetInfo *input : {i0, i1}) {
                    if (input == nullptr)
                        continue;
                    for (auto user : input->users) {
                        if (!user.cell->attrs.count(id_BEL))
                            continue;
                        Loc user_loc =
                                ctx->getBelLocation(ctx->getBelByNameStr(user.cell->attrs.at(id_BEL).as_string()));
                        for (auto bel : ctx->getBels()) {
                            if (ctx->getBelType(bel) != id_ECLKBRIDGECS)
                                continue;
                            loc = ctx->getBelLocation(bel);
                            if (loc.x == user_loc.x) {
                                ci->attrs[id_BEL] = ctx->getBelName(bel).str(ctx);
                                goto eclkbridge_done;
                            }
                        }
                    }
                    if (input->driver.cell != nullptr) {
                        CellInfo *drv = input->driver.cell;
                        if (!drv->attrs.count(id_BEL))
                            continue;
                        Loc drv_loc = ctx->getBelLocation(ctx->getBelByNameStr(drv->attrs.at(id_BEL).as_string()));
                        BelId closest;
                        int closest_x = -1; // aim for same side of chip
                        for (auto bel : ctx->getBels()) {
                            if (ctx->getBelType(bel) != id_ECLKBRIDGECS)
                                continue;
                            loc = ctx->getBelLocation(bel);
                            if (closest_x == -1 || std::abs(loc.x - drv_loc.x) < std::abs(closest_x - drv_loc.x)) {
                                closest_x = loc.x;
                                closest = bel;
                            }
                        }
                        NPNR_ASSERT(closest != BelId());
                        loc = ctx->getBelLocation(closest);
                        ci->attrs[id_BEL] = ctx->getBelName(closest).str(ctx);
                        goto eclkbridge_done;
                    }
                }
                // If all else fails, place randomly
                for (auto bel : ctx->getBels()) {
                    if (ctx->getBelType(bel) != id_ECLKBRIDGECS)
                        continue;
                    loc = ctx->getBelLocation(bel);
                    ci->attrs[id_BEL] = ctx->getBelName(bel).str(ctx);
                }
            eclkbridge_done:
                if (o != nullptr)
                    for (auto user2 : o->users) {
                        // Set side hint to ensure edge clock choice is routeable
                        if (user2.cell->type == id_ECLKSYNCB && user2.port == id_ECLKI) {
                            NetInfo *synco = user2.cell->getPort(id_ECLKO);
                            if (synco != nullptr)
                                bridge_side_hint[synco] = (loc.x > 1) ? 0 : 1;
                        }
                    }
                continue;
            }
        }
        // Promote/route edge clocks
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_IOLOGIC || ci->type == id_DQSBUFM) {
                if (!ci->ports.count(id_ECLK) || ci->ports.at(id_ECLK).net == nullptr)
                    continue;
                BelId bel = ctx->getBelByNameStr(str_or_default(ci->attrs, id_BEL));
                NPNR_ASSERT(bel != BelId());
                Loc pioLoc = ctx->getBelLocation(bel);
                if (ci->type == id_DQSBUFM)
                    pioLoc.z -= 8;
                else
                    pioLoc.z -= 4;
                BelId pioBel = ctx->getBelByLocation(pioLoc);
                NPNR_ASSERT(pioBel != BelId());
                int bank = ctx->get_pio_bel_bank(pioBel);
                make_eclk(ci->ports.at(id_ECLK), ci, bel, bank);
            }
        }
        flush_cells();
        pool<BelId> used_eclksyncb;
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_CLKDIVF) {
                const NetInfo *clki = net_or_nullptr(ci, id_CLKI);
                for (auto &eclk : eclks) {
                    if (eclk.second.unbuf == clki) {
                        for (auto bel : ctx->getBels()) {
                            if (ctx->getBelType(bel) != id_CLKDIVF)
                                continue;
                            Loc loc = ctx->getBelLocation(bel);
                            // CLKDIVF for bank 6/7 on the left; for bank 2/3 on the right
                            if (loc.x < 10 && eclk.first.first != 6 && eclk.first.first != 7)
                                continue;
                            // z-index of CLKDIVF must match index of ECLK
                            if (loc.z != eclk.first.second)
                                continue;
                            ci->attrs[id_BEL] = ctx->getBelName(bel).str(ctx);
                            make_eclk(ci->ports.at(id_CLKI), ci, bel, eclk.first.first);
                            goto clkdiv_done;
                        }
                    }
                }
            clkdiv_done:
                continue;
            } else if (ci->type == id_ECLKSYNCB) {
                const NetInfo *eclki = net_or_nullptr(ci, id_ECLKI);
                const NetInfo *eclko = net_or_nullptr(ci, id_ECLKO);
                if (eclki != nullptr && eclki->driver.cell != nullptr) {
                    if (eclki->driver.cell->type == id_ECLKBRIDGECS) {
                        BelId bel = ctx->getBelByNameStr(eclki->driver.cell->attrs.at(id_BEL).as_string());
                        Loc loc = ctx->getBelLocation(bel);
                        ci->attrs[id_BEL] = ctx->getBelName(ctx->getBelByLocation(Loc(loc.x, loc.y, 15))).str(ctx);
                        used_eclksyncb.insert(bel);
                        goto eclksync_done;
                    }
                }
                if (eclko == nullptr)
                    log_error("ECLKSYNCB '%s' has disconnected port ECLKO\n", ci->name.c_str(ctx));
                for (auto user : eclko->users) {
                    if (user.cell->type == id_TRELLIS_ECLKBUF) {
                        Loc eckbuf_loc =
                                ctx->getBelLocation(ctx->getBelByNameStr(user.cell->attrs.at(id_BEL).as_string()));
                        for (auto bel : ctx->getBels()) {
                            if (ctx->getBelType(bel) != id_ECLKSYNCB)
                                continue;
                            Loc loc = ctx->getBelLocation(bel);
                            if (loc.x == eckbuf_loc.x && loc.y == eckbuf_loc.y && loc.z == eckbuf_loc.z - 2) {
                                ci->attrs[id_BEL] = ctx->getBelName(bel).str(ctx);
                                used_eclksyncb.insert(bel);
                                goto eclksync_done;
                            }
                        }
                    }
                }
            eclksync_done:
                continue;
            } else if (ci->type == id_DDRDLLA) {
                ci->type = id_DDRDLL; // transform from Verilog to Bel name
                const NetInfo *clk = net_or_nullptr(ci, id_CLK);
                if (clk == nullptr)
                    log_error("DDRDLLA '%s' has disconnected port CLK\n", ci->name.c_str(ctx));

                bool left_bank_users = false, right_bank_users = false;
                // Check which side the delay codes (DDRDEL) are used on
                const NetInfo *ddrdel = net_or_nullptr(ci, id_DDRDEL);
                if (ddrdel != nullptr) {
                    for (auto &usr : ddrdel->users) {
                        const CellInfo *uc = usr.cell;
                        if (uc->type != id_DQSBUFM || !uc->attrs.count(id_BEL))
                            continue;
                        BelId dqsb_bel = ctx->getBelByNameStr(uc->attrs.at(id_BEL).as_string());
                        Loc dqsb_loc = ctx->getBelLocation(dqsb_bel);
                        if (dqsb_loc.x > 15)
                            right_bank_users = true;
                        if (dqsb_loc.x < 15)
                            left_bank_users = true;
                    }
                }

                if (left_bank_users && right_bank_users)
                    log_error("DDRDLLA '%s' has DDRDEL uses on both sides of the chip.\n", ctx->nameOf(ci));

                for (auto &eclk : eclks) {
                    if (eclk.second.unbuf == clk) {
                        for (auto bel : ctx->getBels()) {
                            if (ctx->getBelType(bel) != id_DDRDLL)
                                continue;
                            Loc loc = ctx->getBelLocation(bel);
                            if (loc.x > 15 && left_bank_users)
                                continue;
                            if (loc.x < 15 && right_bank_users)
                                continue;
                            int ddrdll_bank = -1;
                            if (loc.x < 15 && loc.y < 15)
                                ddrdll_bank = 7;
                            else if (loc.x < 15 && loc.y > 15)
                                ddrdll_bank = 6;
                            else if (loc.x > 15 && loc.y < 15)
                                ddrdll_bank = 2;
                            else if (loc.x > 15 && loc.y > 15)
                                ddrdll_bank = 3;
                            if (eclk.first.first != ddrdll_bank)
                                continue;
                            log_info("Constraining DDRDLLA '%s' to bel '%s'\n", ctx->nameOf(ci), ctx->nameOfBel(bel));
                            ci->attrs[id_BEL] = ctx->getBelName(bel).str(ctx);
                            make_eclk(ci->ports.at(id_CLK), ci, bel, eclk.first.first);
                            goto ddrdll_done;
                        }
                    }
                }
            ddrdll_done:
                continue;
            }
        }

        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_ECLKSYNCB) {
                // **All** ECLKSYNCBs must be constrained
                // Most will be dealt with above, but there might be some rogue cases
                if (ci->attrs.count(id_BEL))
                    continue;
                for (BelId bel : ctx->getBels()) {
                    if (ctx->getBelType(bel) != id_ECLKSYNCB)
                        continue;
                    // Might there be a better way to pick??
                    if (used_eclksyncb.count(bel))
                        continue;
                    log_info("Constraining ECLKSYNCB '%s' to bel '%s'\n", ctx->nameOf(ci), ctx->nameOfBel(bel));
                    ci->attrs[id_BEL] = ctx->getBelName(bel).str(ctx);
                    goto eclksync_ii_done;
                }
                if (0) {
                eclksync_ii_done:
                    continue;
                }
                log_error("Failed to constrain ECLKSYNCB '%s'\n", ctx->nameOf(ci));
            }
        }

        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_CLKDIVF) {
                if (ci->attrs.count(id_BEL))
                    continue;
                // Case of a CLKDIVF driven by an ECLKSYNC constrained above; without the input being used elsewhere as
                // an edge clock
                const NetInfo *clki = net_or_nullptr(ci, id_CLKI);
                if (clki == nullptr || clki->driver.cell == nullptr)
                    continue;
                CellInfo *drv = clki->driver.cell;
                if (drv->type != id_ECLKSYNCB || !drv->attrs.count(id_BEL))
                    continue;
                BelId bel = ctx->getBelByNameStr(drv->attrs.at(id_BEL).as_string());
                // Find a CLKDIVF that is routeable from the ECLKSYNC
                std::queue<WireId> visit;
                visit.push(ctx->getBelPinWire(bel, id_ECLKO));
                while (!visit.empty()) {
                    WireId cursor = visit.front();
                    visit.pop();
                    for (BelPin bp : ctx->getWireBelPins(cursor)) {
                        if (ctx->getBelType(bp.bel) != id_CLKDIVF || bp.pin != id_CLKI)
                            continue;
                        ci->attrs[id_BEL] = ctx->getBelName(bp.bel).str(ctx);
                        log_info("Constraining CLKDIVF '%s' to bel '%s' based on ECLKSYNCB.\n", ctx->nameOf(ci),
                                 ctx->nameOfBel(bp.bel));
                        goto clkdiv_ii_done;
                    }
                    for (PipId pip : ctx->getPipsDownhill(cursor))
                        visit.push(ctx->getPipDstWire(pip));
                }
            clkdiv_ii_done:
                continue;
            }
        }

        flush_cells();
    };

    void generate_constraints()
    {
        log_info("Generating derived timing constraints...\n");
        auto MHz = [&](delay_t a) { return 1000.0 / ctx->getDelayNS(a); };

        auto equals_epsilon = [](delay_t a, delay_t b) { return (std::abs(a - b) / std::max(double(b), 1.0)) < 1e-3; };

        pool<IdString> user_constrained, changed_nets;
        for (auto &net : ctx->nets) {
            if (net.second->clkconstr != nullptr)
                user_constrained.insert(net.first);
            changed_nets.insert(net.first);
        }
        auto get_period = [&](CellInfo *ci, IdString port, delay_t &period) {
            if (!ci->ports.count(port))
                return false;
            NetInfo *from = ci->ports.at(port).net;
            if (from == nullptr || from->clkconstr == nullptr)
                return false;
            period = from->clkconstr->period.minDelay();
            return true;
        };

        auto set_period = [&](CellInfo *ci, IdString port, delay_t period) {
            if (!ci->ports.count(port))
                return;
            NetInfo *to = ci->ports.at(port).net;
            if (to == nullptr)
                return;
            if (to->clkconstr != nullptr) {
                if (!equals_epsilon(to->clkconstr->period.minDelay(), period) && user_constrained.count(to->name))
                    log_warning(
                            "    Overriding derived constraint of %.1f MHz on net %s with user-specified constraint of "
                            "%.1f MHz.\n",
                            MHz(to->clkconstr->period.min_delay), to->name.c_str(ctx), MHz(period));
                return;
            }
            to->clkconstr = std::unique_ptr<ClockConstraint>(new ClockConstraint());
            to->clkconstr->low = DelayPair(period / 2);
            to->clkconstr->high = DelayPair(period / 2);
            to->clkconstr->period = DelayPair(period);
            log_info("    Derived frequency constraint of %.1f MHz for net %s\n", MHz(to->clkconstr->period.minDelay()),
                     to->name.c_str(ctx));
            changed_nets.insert(to->name);
        };

        auto copy_constraint = [&](CellInfo *ci, IdString fromPort, IdString toPort, double ratio = 1.0) {
            if (!ci->ports.count(fromPort) || !ci->ports.count(toPort))
                return;
            NetInfo *from = ci->ports.at(fromPort).net, *to = ci->ports.at(toPort).net;
            if (from == nullptr || from->clkconstr == nullptr || to == nullptr)
                return;
            if (to->clkconstr != nullptr) {
                if (!equals_epsilon(to->clkconstr->period.minDelay(),
                                    delay_t(from->clkconstr->period.minDelay() / ratio)) &&
                    user_constrained.count(to->name))
                    log_warning(
                            "    Overriding derived constraint of %.1f MHz on net %s with user-specified constraint of "
                            "%.1f MHz.\n",
                            MHz(to->clkconstr->period.minDelay()), to->name.c_str(ctx),
                            MHz(delay_t(from->clkconstr->period.minDelay() / ratio)));
                return;
            }
            to->clkconstr = std::unique_ptr<ClockConstraint>(new ClockConstraint());
            to->clkconstr->low =
                    DelayPair(ctx->getDelayFromNS(ctx->getDelayNS(from->clkconstr->low.min_delay) / ratio));
            to->clkconstr->high =
                    DelayPair(ctx->getDelayFromNS(ctx->getDelayNS(from->clkconstr->high.min_delay) / ratio));
            to->clkconstr->period =
                    DelayPair(ctx->getDelayFromNS(ctx->getDelayNS(from->clkconstr->period.min_delay) / ratio));
            log_info("    Derived frequency constraint of %.1f MHz for net %s\n", MHz(to->clkconstr->period.minDelay()),
                     to->name.c_str(ctx));
            changed_nets.insert(to->name);
        };

        // Run in a loop while constraints are changing to deal with dependencies
        // Iteration limit avoids hanging in crazy loopback situation (self-fed PLLs or dividers, etc)
        int iter = 0;
        const int itermax = 5000;
        while (!changed_nets.empty() && iter < itermax) {
            ++iter;
            pool<IdString> changed_cells;
            for (auto net : changed_nets) {
                for (auto &user : ctx->nets.at(net)->users)
                    if (user.port == id_CLKI || user.port == id_ECLKI || user.port == id_CLK0 || user.port == id_CLK1)
                        changed_cells.insert(user.cell->name);
                auto &drv = ctx->nets.at(net)->driver;
                if (iter == 1 && drv.cell != nullptr && drv.port == id_OSC)
                    changed_cells.insert(drv.cell->name);
            }
            changed_nets.clear();
            for (auto cell : changed_cells) {
                CellInfo *ci = ctx->cells.at(cell).get();
                if (ci->type == id_CLKDIVF) {
                    std::string div = str_or_default(ci->params, id_DIV, "2.0");
                    double ratio;
                    if (div == "2.0")
                        ratio = 1 / 2.0;
                    else if (div == "3.5")
                        ratio = 1 / 3.5;
                    else
                        log_error("Unsupported divider ratio '%s' on CLKDIVF '%s'\n", div.c_str(), ci->name.c_str(ctx));
                    copy_constraint(ci, id_CLKI, id_CDIVX, ratio);
                } else if (ci->type == id_ECLKSYNCB || ci->type == id_TRELLIS_ECLKBUF) {
                    copy_constraint(ci, id_ECLKI, id_ECLKO, 1);
                } else if (ci->type == id_ECLKBRIDGECS) {
                    copy_constraint(ci, id_CLK0, id_ECSOUT, 1);
                    copy_constraint(ci, id_CLK1, id_ECSOUT, 1);
                } else if (ci->type == id_DCCA) {
                    copy_constraint(ci, id_CLKI, id_CLKO, 1);
                } else if (ci->type == id_EHXPLLL) {
                    delay_t period_in;
                    if (!get_period(ci, id_CLKI, period_in))
                        continue;
                    log_info("    Input frequency of PLL '%s' is constrained to %.1f MHz\n", ci->name.c_str(ctx),
                             MHz(period_in));
                    double period_in_div = period_in * int_or_default(ci->params, id_CLKI_DIV, 1);
                    std::string path = str_or_default(ci->params, id_FEEDBK_PATH, "CLKOP");
                    int feedback_div = int_or_default(ci->params, id_CLKFB_DIV, 1);
                    if (path == "CLKOP" || path == "INT_OP")
                        feedback_div *= int_or_default(ci->params, id_CLKOP_DIV, 1);
                    else if (path == "CLKOS" || path == "INT_OS")
                        feedback_div *= int_or_default(ci->params, id_CLKOS_DIV, 1);
                    else if (path == "CLKOS2" || path == "INT_OS2")
                        feedback_div *= int_or_default(ci->params, id_CLKOS2_DIV, 1);
                    else if (path == "CLKOS3" || path == "INT_OS3")
                        feedback_div *= int_or_default(ci->params, id_CLKOS3_DIV, 1);
                    else {
                        log_info("     Unable to determine output frequencies for PLL '%s' with FEEDBK_PATH=%s\n",
                                 ci->name.c_str(ctx), path.c_str());
                        continue;
                    }
                    double vco_period = period_in_div / feedback_div;
                    double vco_freq = MHz(vco_period);
                    if (vco_freq < 400 || vco_freq > 800)
                        log_info("    Derived VCO frequency %.1f MHz of PLL '%s' is out of legal range [400MHz, "
                                 "800MHz]\n",
                                 vco_freq, ci->name.c_str(ctx));
                    set_period(ci, id_CLKOP, vco_period * int_or_default(ci->params, id_CLKOP_DIV, 1));
                    set_period(ci, id_CLKOS, vco_period * int_or_default(ci->params, id_CLKOS_DIV, 1));
                    set_period(ci, id_CLKOS2, vco_period * int_or_default(ci->params, id_CLKOS2_DIV, 1));
                    set_period(ci, id_CLKOS3, vco_period * int_or_default(ci->params, id_CLKOS3_DIV, 1));
                } else if (ci->type == id_OSCG) {
                    int div = int_or_default(ci->params, id_DIV, 128);
                    set_period(ci, id_OSC, delay_t((1.0e6 / (2.0 * 155)) * div));
                }
            }
        }
    }

    void prepack_checks()
    {
        // Check for legacy-style JSON (use CEMUX as a clue) and error out, avoiding a confusing assertion failure
        // later
        for (auto &cell : ctx->cells) {
            if (is_ff(ctx, cell.second.get()) && cell.second->params.count(id_CEMUX) &&
                !cell.second->params[id_CEMUX].is_string)
                log_error("Found netlist using legacy-style JSON parameter values, please update your Yosys.\n");
        }
    }

  public:
    void pack()
    {
        prepack_checks();
        print_logic_usage();
        pack_io();
        pack_dqsbuf();
        preplace_plls();
        pack_iologic();
        pack_ebr();
        pack_dsps();
        pack_dcus();
        pack_misc();
        pack_constants();
        pack_dram();
        pack_carries();
        find_lutff_pairs();
        pack_lut5xs();
        pair_luts();
        pack_lut_pairs();
        pack_remaining_luts();
        pack_remaining_ffs();
        generate_constraints();
        promote_ecp5_globals(ctx);
        ctx->fixupHierarchy();
        ctx->check();
    }

  private:
    Context *ctx;

    pool<IdString> packed_cells;
    std::vector<std::unique_ptr<CellInfo>> new_cells;

    struct SliceUsage
    {
        bool lut0_used = false, lut1_used = false;
        bool ccu2_used = false, dpram_used = false, ramw_used = false;
        bool ff0_used = false, ff1_used = false;
        bool mux5_used = false, muxx_used = false;
    };

    dict<IdString, SliceUsage> sliceUsage;
    dict<IdString, IdString> lutffPairs;
    dict<IdString, IdString> fflutPairs;
    dict<IdString, IdString> lutPairs;
};
// Main pack function
bool Arch::pack()
{
    Context *ctx = getCtx();
    try {
        log_break();
        Ecp5Packer(ctx).pack();
        log_info("Checksum: 0x%08x\n", ctx->checksum());
        assignArchInfo();
        ctx->settings[id_pack] = 1;
        archInfoToAttributes();
        return true;
    } catch (log_execution_error_exception) {
        assignArchInfo();
        return false;
    }
}

void Arch::assignArchInfo()
{
    for (auto &cell : cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type == id_TRELLIS_SLICE) {

            ci->sliceInfo.using_dff = false;
            if (ci->ports.count(id_Q0) && ci->ports[id_Q0].net != nullptr)
                ci->sliceInfo.using_dff = true;
            if (ci->ports.count(id_Q1) && ci->ports[id_Q1].net != nullptr)
                ci->sliceInfo.using_dff = true;

            if (ci->ports.count(id_CLK) && ci->ports[id_CLK].net != nullptr)
                ci->sliceInfo.clk_sig = ci->ports[id_CLK].net->name;
            else
                ci->sliceInfo.clk_sig = IdString();

            if (ci->ports.count(id_LSR) && ci->ports[id_LSR].net != nullptr)
                ci->sliceInfo.lsr_sig = ci->ports[id_LSR].net->name;
            else
                ci->sliceInfo.lsr_sig = IdString();

            ci->sliceInfo.clkmux = id(str_or_default(ci->params, id_CLKMUX, "CLK"));
            ci->sliceInfo.lsrmux = id(str_or_default(ci->params, id_LSRMUX, "LSR"));
            ci->sliceInfo.srmode = id(str_or_default(ci->params, id_SRMODE, "LSR_OVER_CE"));
            std::string mode = str_or_default(ci->params, id_MODE, "LOGIC");
            ci->sliceInfo.is_carry = (mode == "CCU2");
            ci->sliceInfo.is_memory = (mode == "DPRAM" || mode == "RAMW");
            ci->sliceInfo.sd0 = std::stoi(str_or_default(ci->params, id_REG0_SD, "0"));
            ci->sliceInfo.sd1 = std::stoi(str_or_default(ci->params, id_REG1_SD, "0"));
            ci->sliceInfo.has_l6mux = false;
            if (ci->ports.count(id_FXA) && ci->ports[id_FXA].net != nullptr &&
                ci->ports[id_FXA].net->driver.port == id_OFX0)
                ci->sliceInfo.has_l6mux = true;
        } else if (ci->type == id_DP16KD) {
            ci->ramInfo.is_pdp = (int_or_default(ci->params, id_DATA_WIDTH_A, 0) == 36);

            // Output register mode (REGMODE_{A,B}). Valid options are 'NOREG' and 'OUTREG'.
            std::string regmode_a = str_or_default(ci->params, id_REGMODE_A, "NOREG");
            if (regmode_a != "NOREG" && regmode_a != "OUTREG")
                log_error("DP16KD %s has invalid REGMODE_A configuration '%s'\n", ci->name.c_str(this),
                          regmode_a.c_str());
            std::string regmode_b = str_or_default(ci->params, id_REGMODE_B, "NOREG");
            if (regmode_b != "NOREG" && regmode_b != "OUTREG")
                log_error("DP16KD %s has invalid REGMODE_B configuration '%s'\n", ci->name.c_str(this),
                          regmode_b.c_str());
            ci->ramInfo.is_output_a_registered = regmode_a == "OUTREG";
            ci->ramInfo.is_output_b_registered = regmode_b == "OUTREG";

            // Based on the REGMODE, we have different timing lookup tables.
            if (!ci->ramInfo.is_output_a_registered && !ci->ramInfo.is_output_b_registered) {
                ci->ramInfo.regmode_timing_id = id_DP16KD_REGMODE_A_NOREG_REGMODE_B_NOREG;
            } else if (!ci->ramInfo.is_output_a_registered && ci->ramInfo.is_output_b_registered) {
                ci->ramInfo.regmode_timing_id = id_DP16KD_REGMODE_A_NOREG_REGMODE_B_OUTREG;
            } else if (ci->ramInfo.is_output_a_registered && !ci->ramInfo.is_output_b_registered) {
                ci->ramInfo.regmode_timing_id = id_DP16KD_REGMODE_A_OUTREG_REGMODE_B_NOREG;
            } else if (ci->ramInfo.is_output_a_registered && ci->ramInfo.is_output_b_registered) {
                ci->ramInfo.regmode_timing_id = id_DP16KD_REGMODE_A_OUTREG_REGMODE_B_OUTREG;
            }
        } else if (ci->type == id_MULT18X18D) {
            // For the multiplier block, our timing db is dictated by whether any of the input/output registers are
            // enabled. To that end, we need to work out what the parameters are for the INPUTA_CLK, INPUTB_CLK and
            // OUTPUT_CLK are.
            // The clock check is the same IN_A/B and OUT, so hoist it to a function
            auto get_clock_parameter = [&](std::string param_name) {
                std::string clk = str_or_default(ci->params, id(param_name), "NONE");
                if (clk != "NONE" && clk != "CLK0" && clk != "CLK1" && clk != "CLK2" && clk != "CLK3")
                    log_error("MULT18X18D %s has invalid %s configuration '%s'\n", ci->name.c_str(this),
                              param_name.c_str(), clk.c_str());
                return clk;
            };

            // Get the input clock setting from the cell
            std::string reg_inputa_clk = get_clock_parameter("REG_INPUTA_CLK");
            std::string reg_inputb_clk = get_clock_parameter("REG_INPUTB_CLK");

            // Inputs are registered IFF the REG_INPUT value is not NONE
            const bool is_in_a_registered = reg_inputa_clk != "NONE";
            const bool is_in_b_registered = reg_inputb_clk != "NONE";

            // Similarly, get the output register clock
            std::string reg_output_clk = get_clock_parameter("REG_OUTPUT_CLK");
            const bool is_output_registered = reg_output_clk != "NONE";

            // If only one of the inputs is registered, we are going to treat that as
            // neither input registered so that we don't have to deal with mixed timing.
            // Emit a warning to that effect.
            const bool any_input_registered = is_in_a_registered || is_in_b_registered;
            const bool both_inputs_registered = is_in_a_registered && is_in_b_registered;
            const bool input_registers_mismatched = any_input_registered && !both_inputs_registered;
            if (input_registers_mismatched) {
                log_warning("MULT18X18D %s has unsupported mixed input register modes (reg_inputa_clk=%s, "
                            "reg_inputb_clk=%s)\n",
                            ci->name.c_str(this), reg_inputa_clk.c_str(), reg_inputb_clk.c_str());
                log_warning("Timings for MULT18X18D %s will be calculated as though neither input were registered\n",
                            ci->name.c_str(this));

                // Act as though the inputs are unregistered, so select timing DB based only on the
                // output register mode
                ci->multInfo.timing_id = is_output_registered ? id_MULT18X18D_REGS_OUTPUT : id_MULT18X18D_REGS_NONE;
            } else {
                // Based on our register settings, pick the timing data to use for this cell
                if (!both_inputs_registered && !is_output_registered) {
                    ci->multInfo.timing_id = id_MULT18X18D_REGS_NONE;
                } else if (both_inputs_registered && !is_output_registered) {
                    ci->multInfo.timing_id = id_MULT18X18D_REGS_INPUT;
                } else if (!both_inputs_registered && is_output_registered) {
                    ci->multInfo.timing_id = id_MULT18X18D_REGS_OUTPUT;
                } else if (both_inputs_registered && is_output_registered) {
                    ci->multInfo.timing_id = id_MULT18X18D_REGS_ALL;
                }
            }
            // If we aren't a pure combinatorial multiplier, then our timings are
            // calculated with respect to CLK0
            ci->multInfo.is_clocked = ci->multInfo.timing_id != id_MULT18X18D_REGS_NONE;
        }
    }
    for (auto &net : nets) {
        net.second->is_global = bool_or_default(net.second->attrs, id_ECP5_IS_GLOBAL);
    }
}

NEXTPNR_NAMESPACE_END
