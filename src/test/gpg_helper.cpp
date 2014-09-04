/**
 * asymmetricfs - An asymmetric encryption-aware filesystem
 * (c) 2014 Chris Kennelly <chris@ckennelly.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <boost/lexical_cast.hpp>
#include "gpg_helper.h"
#include <sstream>
#include "subprocess.h"


gnupg_error::~gnupg_error() {}

gnupg_generation_error::gnupg_generation_error(const std::string& message) :
    what_(message) {}

gnupg_generation_error::~gnupg_generation_error() {}

const std::string& gnupg_generation_error::what() const {
    return what_;
}

gnupg_key::gnupg_key(const key_specification& spec) : spec_(spec),
        public_keyring_(key_directory_.path() / "pubring.gpg"),
        secret_keyring_(key_directory_.path() / "secring.gpg") {
    std::stringstream batch;

    batch << "Key-Type: RSA" << std::endl;
    batch << "Key-Length: " << spec.key_size << std::endl;
    batch << "Subkey-Type: default" << std::endl;

    if (!(spec_.name.empty())) {
        batch << "Name-Real: " << spec_.name << std::endl;
    }

    if (!(spec_.email.empty())) {
        batch << "Name-Email: " << spec_.email << std::endl;
    }

    if (!(spec_.comment.empty())) {
        batch << "Name-Comment: " << spec_.comment << std::endl;
    }

    // operator<< quotes the contents of boost::filesystem::path objects,
    // leading GPG to fail.  We convert to a string first, avoiding the
    // automatic quoting.
    batch << "%pubring " << public_keyring_.string() << std::endl;
    batch << "%secring " << secret_keyring_.string() << std::endl;
    batch << "%no-protection" << std::endl;
    batch << "%transient-key" << std::endl;
    batch << "%commit" << std::endl;

    std::string command = batch.str();
    size_t in_size = command.size();

    {
        const std::vector<std::string> argv{
            "gpg",
            "--gen-key",
            "--batch",
            "--no-tty",
            "--no-default-keyring",
            "--no-permission-warning",
            // TODO: Use --quick-random on GPG 1.x
            "--debug-quick-random"};
        subprocess p(-1, -1, "gpg", argv);

        int ret;
        ret = p.communicate(nullptr, nullptr, command.c_str(), &in_size);
        if (ret != 0) {
            throw gnupg_generation_error("Unable to communicate with GPG.");
        }

        ret = p.wait();
        if (ret != 0) {
            throw gnupg_generation_error("GPG exited with an error.");
        }
    }

    // Look up key thumbprint.
    std::string buffer(1 << 12, '\0');
    {
        const std::vector<std::string> argv{
            "gpg",
            "--homedir",
            key_directory_.path().string(),
            "--no-permission-warning",
            "--fingerprint"};
        subprocess p(-1, -1, "gpg", argv);

        size_t buffer_size = buffer.size();

        int ret;
        ret = p.communicate(&buffer[0], &buffer_size, nullptr, nullptr);
        if (ret != 0) {
            throw gnupg_generation_error("Unable to communicate with GPG.");
        }
        // Truncate.  buffer_size now tells us how many bytes are remaining in
        // buffer *after* the actual data.
        buffer.resize(buffer.size() - buffer_size);

        ret = p.wait();
        if (ret != 0) {
            throw gnupg_generation_error("GPG exited with an error.");
        }
    }

    const std::string key_size(boost::lexical_cast<std::string>(spec.key_size));
    const std::string key_token = "pub   " + key_size + "R/";
    size_t index = buffer.find(key_token);
    if (index == std::string::npos ||
            index + key_token.size() + 8 > buffer.size()) {
        throw gnupg_generation_error("Unable to locate fingerprint.");
    }

    thumbprint_ = buffer.substr(index + key_token.size(), 8);
}

gnupg_key::~gnupg_key() {}

boost::filesystem::path gnupg_key::public_keyring() const {
    return public_keyring_;
}

boost::filesystem::path gnupg_key::secret_keyring() const {
    return secret_keyring_;
}

boost::filesystem::path gnupg_key::home() const {
    return key_directory_.path();
}

gpg_recipient gnupg_key::thumbprint() const {
    return gpg_recipient(thumbprint_);
}
