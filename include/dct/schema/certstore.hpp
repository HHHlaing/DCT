#ifndef CERTSTORE_HPP
#define CERTSTORE_HPP
/*
 * Certificate store abstraction used by schemas
 *
 * Copyright (C) 2020-2 Pollere LLC
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <https://www.gnu.org/licenses/>.
 *  You may contact Pollere LLC at info@pollere.net.
 *
 *  The DCT proof-of-concept is not intended as production code.
 *  More information on DCT is available from info@pollere.net
 */

#include <algorithm>
#include <set>
#include <span>
#include <string>
#include <vector>
#include "bschema.hpp"
#include "dct_cert.hpp"

using schema_error = bschema::schema_error;

using certName = ndn::Name;
using certVec = std::vector<certName>;
using certChain = std::vector<thumbPrint>;
using keyVal = std::vector<uint8_t>;
using certAddCb = std::function<void(const dctCert&)>;
using chainAddCb = std::function<void(const dctCert&)>;

struct certStore {
    std::unordered_map<thumbPrint,dctCert> certs_{}; // validated certs
    std::unordered_map<thumbPrint,keyVal> key_{};    // cert-to-key (for signing certs)
    certChain chains_{}; // array of signing chain heads (thumbprints of signing certs)
    certAddCb addCb_{[](const dctCert&){}};          // called when a cert is added
    chainAddCb chainAddCb_{[](const dctCert&){}};    // called when a new signing chain is added
    static constexpr thumbPrint ztp_{};              // self-signed cert's thumbprint

    void dumpcerts() const {
        print("Cert Dump\n");
        int i = 0;
        for (const auto& [tp, cert] : certs_) print("{} {} tp {:x}\n", i++, cert.getName().toUri(), fmt::join(tp," "));
    }

    auto begin() const { return certs_.cbegin(); }
    auto end() const { return certs_.cend(); }

    // lookup a cert given its thumbprint
    const dctCert& get(const thumbPrint& tp) const { return certs_.at(tp); }
    const auto& operator[](const thumbPrint& tp) const { return get(tp); }

    auto contains(const thumbPrint& tp) const noexcept { return certs_.contains(tp); }

    // lookup the signing cert of 'data'
    const auto& operator[](rData data) const {
        const auto& tp = dctCert::getKeyLoc(data);
        if (dctCert::selfSigned(tp)) {
            throw schema_error(format("looking up self-signed {}", data.name())); //XXX
            //return data;
        }
        return get(tp);
    }
    const auto& operator[](const ndn::Data& data) const {
         const auto& tp = dctCert::getKeyLoc(data);
         return dctCert::selfSigned(tp)? reinterpret_cast<const dctCert&>(data) : get(tp);
    }

    // lookup the (public) signing key of 'data'
    auto signingKey(rData data) const {
        const auto& tp = dctCert::getKeyLoc(data);
        if (! dctCert::selfSigned(tp)) data = rData(get(tp)); //XXX fix when get returns rData
        return data.content().rest();
    }

    const auto& key(const thumbPrint& tp) const { return key_.at(tp); }
    auto canSign(const thumbPrint& tp) const { return key_.contains(tp); }

    auto finishAdd(auto it) {
        if (it.second) {
            const auto& [tp, cert] = *it.first;
            addCb_(cert);
        }
        return it;
    }
    // Routines to add a cert to the store. The first two add non-signing certs.
    // The third adds a signing cert with its secret key. If the thumbprint is
    // already in the store, nothing is added or changed (since the thumbprint
    // is a 1-1 mapping to its cert, it should be an error for the mapping
    // to change but this is not currently checked).
    // All return an <iterator,status> pair the points to the element added with
    // 'status' true if the element was added and false if it was already there.
    auto add(const dctCert& c) { return finishAdd(certs_.try_emplace(c.computeThumbPrint(), c)); }
    auto add(dctCert&& c) { return finishAdd(certs_.try_emplace(c.computeThumbPrint(), std::move(c))); }

    auto add(const dctCert& c, const keyVal& k) {
        auto it = finishAdd(certs_.try_emplace(c.computeThumbPrint(), c));
        if (k.size() && it.second) {
            const auto& [tp, cert] = *it.first;
            key_.try_emplace(tp, k);
        }
        return it;
    }

    struct chainIter {
        const thumbPrint* tp_;
        const certStore& cs_;

        chainIter(const thumbPrint& tp, const certStore& cs) : tp_{&tp}, cs_{cs} {}
        constexpr bool operator!=(const thumbPrint& tp) const { return *tp_ != tp; }
        constexpr void operator++() const { }
        const auto& operator*() {
            const auto& c = cs_[*tp_];
            tp_ = &c.getKeyLoc();
            return c;
        }
        constexpr auto& begin() const { return *this; }
        constexpr auto& end() const { return ztp_; }
    };

    // for the signing chain starting with 'tp', return the first element that satisfies 'pred'
    template<typename Pred>
    auto chainMatch(const thumbPrint& tp, Pred&& pred) const {
        for (const auto& cert: chainIter(tp, *this)) if (rData c{cert}; pred(c)) return std::pair{true, c};
        return std::pair{false, rData{}};
    }

    // construct a vector of the names of each cert in cert's signing chain.
    certVec chainNames(const dctCert& cert) const {
        certVec cv{};
        cv.emplace_back(cert.getName());
        for (const auto& c: chainIter(cert.getKeyLoc(),*this)) cv.emplace_back(c.getName());
        return cv;
    }

    auto signingChain() const { return chains_.empty()? certVec{} : chainNames(get(chains_[0])); } //XXX

    // return the trust anchor thumbprint of signing chain 'idx'.
    const auto& trustAnchorTP(size_t idx) const {
        if (chains_.empty()) throw schema_error(format("trustAnchorTP: signing chain {} doesn't exist", idx));
        const auto* ltp = &ztp_;
        for (const auto* tp = &chains_[idx]; !dctCert::selfSigned(*tp); tp = &get(*tp).getKeyLoc()) { ltp = tp; }
        return *ltp;
    }

    void addChain(const dctCert& cert) {
        chains_.emplace_back(cert.computeThumbPrint());
        chainAddCb_(cert);
    }
    const auto& Chains() const { return chains_; }
};

#endif // CERTSTORE_HPP
