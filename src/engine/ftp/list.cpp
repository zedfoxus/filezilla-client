#include <filezilla.h>

#include "../directorycache.h"
#include "../servercapabilities.h"
#include "list.h"
#include "transfersocket.h"

namespace {
// Some servers are broken. Instead of an empty listing, some MVS servers
// for example they return "550 no members found"
// Other servers return "550 No files found."
bool IsMisleadingListResponse(std::wstring const& response)
{
	// Some servers are broken. Instead of an empty listing, some MVS servers
	// for example they return "550 no members found"
	// Other servers return "550 No files found."

	if (!fz::stricmp(response, L"550 No members found.")) {
		return true;
	}

	if (!fz::stricmp(response, L"550 No data sets found.")) {
		return true;
	}

	if (fz::str_tolower_ascii(response) == L"550 no files found.") {
		return true;
	}

	return false;
}
}

CFtpListOpData::CFtpListOpData(CFtpControlSocket & controlSocket, CServerPath const& path, std::wstring const& subDir, int flags)
    : COpData(Command::list)
    , CFtpOpData(controlSocket)
    , path_(path)
    , subDir_(subDir)
    , flags_(flags)
{
	if (path_.GetType() == DEFAULT) {
		path_.SetType(currentServer().GetType());
	}
	refresh = (flags & LIST_FLAG_REFRESH) != 0;
	fallback_to_current = !path.empty() && (flags & LIST_FLAG_FALLBACK_CURRENT) != 0;
}

int CFtpListOpData::Send()
{
	controlSocket_.LogMessage(MessageType::Debug_Verbose, L"CFtpListOpData::ListSend()");
	controlSocket_.LogMessage(MessageType::Debug_Debug, L"  state = %d", opState);

	if (opState == list_waitcwd) {
		controlSocket_.ChangeDir(path_, subDir_, (flags_ & LIST_FLAG_LINK));
		return FZ_REPLY_CONTINUE;
	}
	if (opState == list_waitlock) {
		assert(subDir_.empty()); // We did do ChangeDir before trying to lock

		// Check if we can use already existing listing
		CDirectoryListing listing;
		bool is_outdated = false;
		bool found = controlSocket_.engine_.GetDirectoryCache().Lookup(listing, currentServer(), controlSocket_.m_CurrentPath, true, is_outdated);
		if (found && !is_outdated && !listing.get_unsure_flags() &&
			(!refresh || (holdsLock && listing.m_firstListTime >= m_time_before_locking)))
		{
			controlSocket_.SendDirectoryListingNotification(controlSocket_.m_CurrentPath, !pNextOpData, false);
			return FZ_REPLY_OK;
		}

		if (!holdsLock) {
			if (!controlSocket_.TryLockCache(CFtpControlSocket::lock_list, controlSocket_.m_CurrentPath)) {
				m_time_before_locking = fz::monotonic_clock::now();
				return FZ_REPLY_WOULDBLOCK;
			}
		}

		controlSocket_.m_pTransferSocket.reset();
		controlSocket_.m_pTransferSocket = std::make_unique<CTransferSocket>(controlSocket_.engine_, controlSocket_, TransferMode::list);

		// Assume that a server supporting UTF-8 does not send EBCDIC listings.
		listingEncoding::type encoding = listingEncoding::unknown;
		if (CServerCapabilities::GetCapability(currentServer(), utf8_command) == yes) {
			encoding = listingEncoding::normal;
		}

		m_pDirectoryListingParser = std::make_unique<CDirectoryListingParser>(&controlSocket_, currentServer(), encoding);

		m_pDirectoryListingParser->SetTimezoneOffset(controlSocket_.GetTimezoneOffset());
		controlSocket_.m_pTransferSocket->m_pDirectoryListingParser = m_pDirectoryListingParser.get();

		controlSocket_.engine_.transfer_status_.Init(-1, 0, true);

		opState = list_waittransfer;
		if (CServerCapabilities::GetCapability(currentServer(), mlsd_command) == yes) {
			controlSocket_.Transfer(L"MLSD", this);
			return FZ_REPLY_CONTINUE;
		}
		else {
			if (controlSocket_.engine_.GetOptions().GetOptionVal(OPTION_VIEW_HIDDEN_FILES)) {
				capabilities cap = CServerCapabilities::GetCapability(currentServer(), list_hidden_support);
				if (cap == unknown) {
					viewHiddenCheck = true;
				}
				else if (cap == yes) {
					viewHidden = true;
				}
				else {
					LogMessage(MessageType::Debug_Info, _("View hidden option set, but unsupported by server"));
				}
			}

			if (viewHidden) {
				controlSocket_.Transfer(L"LIST -a", this);
				return FZ_REPLY_CONTINUE;
			}
			else {
				controlSocket_.Transfer(L"LIST", this);
				return FZ_REPLY_CONTINUE;
			}
		}
	}
	if (opState == list_mdtm) {
		LogMessage(MessageType::Status, _("Calculating timezone offset of server..."));
		std::wstring cmd = L"MDTM " + controlSocket_.m_CurrentPath.FormatFilename(directoryListing[mdtm_index].name, true);
		if (!controlSocket_.SendCommand(cmd)) {
			return FZ_REPLY_ERROR;
		}
		else {
			return FZ_REPLY_WOULDBLOCK;
		}
	}

	LogMessage(MessageType::Debug_Warning, L"invalid opstate %d", opState);
	return FZ_REPLY_INTERNALERROR;
}


int CFtpListOpData::ParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpListOpData::ParseResponse()");

	if (opState != list_mdtm) {
		LogMessage(MessageType::Debug_Warning, "CFtpListOpData::ParseResponse should never be called if opState != list_mdtm");
		return FZ_REPLY_INTERNALERROR;
	}

	std::wstring const& response = controlSocket_.m_Response;

	// First condition prevents problems with concurrent MDTM
	if (CServerCapabilities::GetCapability(currentServer(), timezone_offset) == unknown &&
	    response.substr(0, 4) == L"213 " && response.size() > 16)
	{
		fz::datetime date(response.substr(4), fz::datetime::utc);
		if (!date.empty()) {
			assert(directoryListing[mdtm_index].has_date());
			fz::datetime listTime = directoryListing[mdtm_index].time;
			listTime -= fz::duration::from_minutes(currentServer().GetTimezoneOffset());

			int serveroffset = static_cast<int>((date - listTime).get_seconds());
			if (!directoryListing[mdtm_index].has_seconds()) {
				// Round offset to full minutes
				if (serveroffset < 0) {
					serveroffset -= 59;
				}
				serveroffset -= serveroffset % 60;
			}

			LogMessage(MessageType::Status, L"Timezone offset of server is %d seconds.", -serveroffset);

			fz::duration span = fz::duration::from_seconds(serveroffset);
			const int count = directoryListing.GetCount();
			for (int i = 0; i < count; ++i) {
				CDirentry& entry = directoryListing.get(i);
				entry.time += span;
			}

			// TODO: Correct cached listings

			CServerCapabilities::SetCapability(currentServer(), timezone_offset, yes, serveroffset);
		}
		else {
			CServerCapabilities::SetCapability(currentServer(), mdtm_command, no);
			CServerCapabilities::SetCapability(currentServer(), timezone_offset, no);
		}
	}
	else {
		CServerCapabilities::SetCapability(currentServer(), timezone_offset, no);
	}

	controlSocket_.engine_.GetDirectoryCache().Store(directoryListing, currentServer());

	controlSocket_.SendDirectoryListingNotification(controlSocket_.m_CurrentPath, !pNextOpData, false);

	return FZ_REPLY_OK;
}


int CFtpListOpData::SubcommandResult(int prevResult, COpData const& previousOperation)
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpListOpData::SubcommandResult()");

	LogMessage(MessageType::Debug_Debug, L"  state = %d", opState);

	if (opState == list_waitcwd) {
		if (prevResult != FZ_REPLY_OK) {
			if ((prevResult & FZ_REPLY_LINKNOTDIR) == FZ_REPLY_LINKNOTDIR) {
				return prevResult;
			}

			if (fallback_to_current) {
				// List current directory instead
				fallback_to_current = false;
				path_.clear();
				subDir_.clear();
				controlSocket_.ChangeDir();
				return FZ_REPLY_CONTINUE;
			}
			else {
				return prevResult;
			}
		}
		if (path_.empty()) {
			path_ = controlSocket_.m_CurrentPath;
			assert(subDir_.empty());
			assert(!path_.empty());
		}
		opState = list_waitlock;
		return FZ_REPLY_CONTINUE;
	}
	else if (opState == list_waittransfer) {
		if (prevResult == FZ_REPLY_OK) {
			CDirectoryListing listing = m_pDirectoryListingParser->Parse(controlSocket_.m_CurrentPath);

			if (viewHiddenCheck) {
				if (!viewHidden) {
					// Repeat with LIST -a
					viewHidden = true;
					directoryListing = listing;

					// Reset status
					transferEndReason = TransferEndReason::successful;
					tranferCommandSent = false;
					controlSocket_.m_pTransferSocket.reset();
					controlSocket_.m_pTransferSocket = std::make_unique<CTransferSocket>(controlSocket_.engine_, controlSocket_, TransferMode::list);
					m_pDirectoryListingParser->Reset();
					controlSocket_.m_pTransferSocket->m_pDirectoryListingParser = m_pDirectoryListingParser.get();

					controlSocket_.Transfer(L"LIST -a", this);
					return FZ_REPLY_CONTINUE;
				}
				else {
					if (CheckInclusion(listing, directoryListing)) {
						LogMessage(MessageType::Debug_Info, L"Server seems to support LIST -a");
						CServerCapabilities::SetCapability(currentServer(), list_hidden_support, yes);
					}
					else {
						LogMessage(MessageType::Debug_Info, L"Server does not seem to support LIST -a");
						CServerCapabilities::SetCapability(currentServer(), list_hidden_support, no);
						listing = directoryListing;
					}
				}
			}

			controlSocket_.SetAlive();

			int res = CheckTimezoneDetection(listing);
			if (res != FZ_REPLY_OK) {
				return res;
			}

			controlSocket_.engine_.GetDirectoryCache().Store(listing, currentServer());

			controlSocket_.SendDirectoryListingNotification(controlSocket_.m_CurrentPath, !pNextOpData, false);

			return FZ_REPLY_OK;
		}
		else {
			if (tranferCommandSent && IsMisleadingListResponse(controlSocket_.m_Response)) {
				CDirectoryListing listing;
				listing.path = controlSocket_.m_CurrentPath;
				listing.m_firstListTime = fz::monotonic_clock::now();

				if (viewHiddenCheck) {
					if (viewHidden) {
						if (directoryListing.GetCount()) {
							// Less files with LIST -a
							// Not supported
							LogMessage(MessageType::Debug_Info, L"Server does not seem to support LIST -a");
							CServerCapabilities::SetCapability(currentServer(), list_hidden_support, no);
							listing = directoryListing;
						}
						else {
							LogMessage(MessageType::Debug_Info, L"Server seems to support LIST -a");
							CServerCapabilities::SetCapability(currentServer(), list_hidden_support, yes);
						}
					}
					else {
						// Reset status
						transferEndReason = TransferEndReason::successful;
						tranferCommandSent = false;
						controlSocket_.m_pTransferSocket.reset();
						controlSocket_.m_pTransferSocket = std::make_unique<CTransferSocket>(controlSocket_.engine_, controlSocket_, TransferMode::list);
						m_pDirectoryListingParser->Reset();
						controlSocket_.m_pTransferSocket->m_pDirectoryListingParser = m_pDirectoryListingParser.get();

						// Repeat with LIST -a
						viewHidden = true;
						directoryListing = listing;
						controlSocket_.Transfer(L"LIST -a", this);
						return FZ_REPLY_CONTINUE;
					}
				}

				int res = CheckTimezoneDetection(listing);
				if (res != FZ_REPLY_OK) {
					return res;
				}

				controlSocket_.engine_.GetDirectoryCache().Store(listing, currentServer());

				controlSocket_.SendDirectoryListingNotification(controlSocket_.m_CurrentPath, !pNextOpData, false);

				return FZ_REPLY_OK;
			}
			else {
				if (viewHiddenCheck) {
					// If server does not support LIST -a, the server might reject this command
					// straight away. In this case, back to the previously retrieved listing.
					// On other failures like timeouts and such, return an error
					if (viewHidden &&
						transferEndReason == TransferEndReason::transfer_command_failure_immediate)
					{
						CServerCapabilities::SetCapability(currentServer(), list_hidden_support, no);

						int res = CheckTimezoneDetection(directoryListing);
						if (res != FZ_REPLY_OK) {
							return res;
						}

						controlSocket_.engine_.GetDirectoryCache().Store(directoryListing, currentServer());

						controlSocket_.SendDirectoryListingNotification(controlSocket_.m_CurrentPath, !pNextOpData, false);

						return FZ_REPLY_OK;
					}
				}

				if (prevResult & FZ_REPLY_ERROR) {
					controlSocket_.SendDirectoryListingNotification(controlSocket_.m_CurrentPath, !pNextOpData, true);
				}
			}

			return FZ_REPLY_ERROR;
		}
	}
	else {
		LogMessage(MessageType::Debug_Warning, L"Wrong opState: %d", opState);
		return FZ_REPLY_INTERNALERROR;
	}
}

int CFtpListOpData::CheckTimezoneDetection(CDirectoryListing& listing)
{
	if (CServerCapabilities::GetCapability(currentServer(), timezone_offset) == unknown) {
		if (CServerCapabilities::GetCapability(currentServer(), mdtm_command) != yes) {
			CServerCapabilities::SetCapability(currentServer(), timezone_offset, no);
		}
		else {
			const int count = listing.GetCount();
			for (int i = 0; i < count; ++i) {
				if (!listing[i].is_dir() && listing[i].has_time()) {
					opState = list_mdtm;
					directoryListing = listing;
					mdtm_index = i;
					return FZ_REPLY_CONTINUE;
				}
			}
		}
	}

	return FZ_REPLY_OK;
}
