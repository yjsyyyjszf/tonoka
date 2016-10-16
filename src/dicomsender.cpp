
#include "dicomsender.h"
#include <boost/lexical_cast.hpp>
#include <boost/thread.hpp>
#include "destinationentry.h"

// work around the fact that dcmtk doesn't work in unicode mode, so all string operation needs to be converted from/to mbcs
#ifdef _UNICODE
#undef _UNICODE
#undef UNICODE
#define _UNDEFINEDUNICODE
#endif

#include "dcmtk/ofstd/ofstd.h"
#include "dcmtk/oflog/oflog.h"
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmnet/dimse.h"
#include "dcmtk/dcmnet/diutil.h"
#include "dcmtk/dcmnet/scu.h"

#ifdef _UNDEFINEDUNICODE
#define _UNICODE 1
#define UNICODE 1
#endif



DICOMSender::DICOMSender(PatientData &patientdata) 
	: patientdata(patientdata)
{			
	cancelEvent = doneEvent = false;
}

DICOMSender::~DICOMSender()
{
}

void DICOMSender::DoSendAsync(DestinationEntry destination, int threads)
{
	SetDone(false);
	ClearCancel();
	
	this->m_destination = destination;	
	this->m_threads = threads;

	// start the thread, let the sender manage (e.g. cancel), so we don't need to track anymore
	boost::thread t(DICOMSender::DoSendThread, this);
	t.detach();
}


void DICOMSender::DoSendThread(void *obj)
{
	DICOMSender *me = (DICOMSender *) obj;

	OFLog::configure(OFLogger::OFF_LOG_LEVEL);	

	if (me)
	{		
		me->DoSend(me->m_destination, me->m_threads);
		me->SetDone(true);	
	}
}

void DICOMSender::DoSend(DestinationEntry destination, int threads)
{
	OFLog::configure(OFLogger::OFF_LOG_LEVEL);
	
	// get a list of files	
	study_dirs.clear();
	service.reset();
	patientdata.GetStudies(boost::bind(&DICOMSender::fillstudies, this, _1));
	for (std::vector<boost::filesystem::path>::iterator it = study_dirs.begin(); it != study_dirs.end(); ++it)
	{
		// post them in the work
		service.post(boost::bind(&DICOMSender::SendStudy, this, *it));
	}

	// run 5 threads
	boost::thread_group threadgroup;
	for (int i = 0; i < threads; i++)
		threadgroup.create_thread(boost::bind(&boost::asio::io_service::run, &service));

	// wait for everything to finish, Cancel() calls service stop and stops farther work from processing
	threadgroup.join_all();
}

int DICOMSender::fillstudies(Study &study)
{
	// only put in 
	if (study.checked)
		study_dirs.push_back(study.path);
	return 0;
}

void DICOMSender::SendStudy(boost::filesystem::path path)
{
	// each instance of this function is a thread, don't write to class members!
	int retry = 0;
	int unsentcountbefore = 0;
	int unsentcountafter = 0;
	mapset sopclassuidtransfersyntax;
	naturalpathmap instances;	// sopid, filename, this ensures we send out instances in sopid order	
	std::string study_uid;

	// scan the directory for all instances in the study
	ScanDir(path, instances, sopclassuidtransfersyntax, study_uid);

	do
	{
		// get number of unsent images
		unsentcountbefore = instances.size();

		// batch send
		if (unsentcountbefore > 0)
			SendABatch(sopclassuidtransfersyntax, instances);

		unsentcountafter = instances.size();
		
		// only do a sleep if there's more to send, we didn't send anything out, and we still want to retry
		if (unsentcountafter > 0 && unsentcountbefore == unsentcountafter && retry < 10000)
		{
			retry++;			

			// sleep loop with cancel check, 1 minutes
			int sleeploop = 5 * 60 * 1;
			while (sleeploop > 0)
			{
#ifdef _WIN32
				Sleep(200);
#else
                usleep(200);
#endif
                sleeploop--;
				if (IsCanceled())
					break;
			}
		}
		else		// otherwise, the next loop is not a retry
		{
			retry = 0;
		}
	}
	while (!IsCanceled() && unsentcountafter > 0 && retry < 10000);

	if (unsentcountafter == 0)
		patientdata.SetStudyCheck(study_uid, false);
}

int DICOMSender::SendABatch(const mapset &sopclassuidtransfersyntax, naturalpathmap &instances)
{		
	DcmSCU scu;

	if (IsCanceled())
		return 1;

	scu.setVerbosePCMode(true);
	scu.setAETitle(m_destination.ourAETitle.c_str());
	scu.setPeerHostName(m_destination.destinationHost.c_str());
	scu.setPeerPort(m_destination.destinationPort);
	scu.setPeerAETitle(m_destination.destinationAETitle.c_str());
	scu.setACSETimeout(30);
	scu.setDIMSETimeout(60);
	scu.setDatasetConversionMode(true);

	OFList<OFString> defaulttransfersyntax;
	defaulttransfersyntax.push_back(UID_LittleEndianExplicitTransferSyntax);

	// for every class..
	for (mapset::const_iterator it = sopclassuidtransfersyntax.begin(); it != sopclassuidtransfersyntax.end(); it++)
	{
		// make list of what's in the file, and propose it first.  default proposed as a seperate context
		OFList<OFString> transfersyntax;
		for(std::set<std::string>::iterator it2 = it->second.begin(); it2 != it->second.end(); it2++)
		{
			if(*it2 != UID_LittleEndianExplicitTransferSyntax)
				transfersyntax.push_back(it2->c_str());
		}

		if(transfersyntax.size() > 0)
			scu.addPresentationContext(it->first.c_str(), transfersyntax);

		// propose the default UID_LittleEndianExplicitTransferSyntax
		scu.addPresentationContext(it->first.c_str(), defaulttransfersyntax);
	}
	
	OFCondition cond;
	
	if(scu.initNetwork().bad())
		return 1;

	if(scu.negotiateAssociation().bad())
		return 1;

	naturalpathmap::iterator itr = instances.begin();
	while(itr != instances.end())
	{
		if(IsCanceled())
		{			
			break;
		}

		Uint16 status;		

		// load file
		DcmFileFormat dcmff;
		dcmff.loadFile(itr->second.c_str());

		// do some precheck of the transfer syntax
		DcmXfer fileTransfer(dcmff.getDataset()->getOriginalXfer());
		OFString sopclassuid;
		dcmff.getDataset()->findAndGetOFString(DCM_SOPClassUID, sopclassuid);
	
		// out found.. change to 
		T_ASC_PresentationContextID pid = scu.findAnyPresentationContextID(sopclassuid, fileTransfer.getXferID());
		
		cond = scu.sendSTORERequest(pid, "", dcmff.getDataset(), status);
		if(cond.good())
			instances.erase(itr++);
		else if(cond == DUL_PEERABORTEDASSOCIATION)
			return 1;
		else			// some error? keep going
		{		
			itr++;
		}		
	}

	scu.releaseAssociation();	
	return 0;
}

void DICOMSender::ScanDir(boost::filesystem::path path, naturalpathmap &instances, mapset &sopclassuidtransfersyntax, std::string &study_uid)
{
	boost::filesystem::path someDir(path);
	boost::filesystem::directory_iterator end_iter;

	if (boost::filesystem::exists(someDir) && boost::filesystem::is_directory(someDir))
	{
		for (boost::filesystem::directory_iterator dir_iter(someDir); dir_iter != end_iter; dir_iter++)
		{
			if (IsCanceled())
			{
				break;
			}

			if (boost::filesystem::is_regular_file(dir_iter->status()))
			{
				ScanFile(*dir_iter, instances, sopclassuidtransfersyntax, study_uid);
			}
			else if (boost::filesystem::is_directory(dir_iter->status()))
			{
				// descent recursively
				ScanDir(*dir_iter, instances, sopclassuidtransfersyntax, study_uid);
			}
		}
	}
}

void DICOMSender::ScanFile(boost::filesystem::path path, naturalpathmap &instances, mapset &sopclassuidtransfersyntax, std::string &study_uid)
{
	DcmFileFormat dfile;
	OFCondition cond = dfile.loadFile(path.c_str());
	if (cond.good())
	{		
		OFString studyuid;		
		OFString sopuid, sopclassuid, transfersyntax;
		
		dfile.getDataset()->findAndGetOFString(DCM_StudyInstanceUID, studyuid);

		dfile.getDataset()->findAndGetOFString(DCM_SOPInstanceUID, sopuid);
		dfile.getDataset()->findAndGetOFString(DCM_SOPClassUID, sopclassuid);
		DcmXfer filexfer(dfile.getDataset()->getOriginalXfer());
		transfersyntax = filexfer.getXferID();
	
		// add file to send
		instances.insert(std::pair<std::string, boost::filesystem::path>(sopuid.c_str(), path));

		// remember the class and transfersyntax
		sopclassuidtransfersyntax[sopclassuid.c_str()].insert(transfersyntax.c_str());

		study_uid = studyuid.c_str();
	}

}

bool DICOMSender::Echo(DestinationEntry destination)
{
	DcmSCU scu;

	scu.setVerbosePCMode(true);
	scu.setAETitle(destination.ourAETitle.c_str());
	scu.setPeerHostName(destination.destinationHost.c_str());
	scu.setPeerPort(destination.destinationPort);
	scu.setPeerAETitle(destination.destinationAETitle.c_str());
	scu.setACSETimeout(30);
	scu.setDIMSETimeout(60);
	scu.setDatasetConversionMode(true);
	
	OFList<OFString> transfersyntax;
	transfersyntax.push_back(UID_LittleEndianExplicitTransferSyntax);
	transfersyntax.push_back(UID_LittleEndianImplicitTransferSyntax);
	scu.addPresentationContext(UID_VerificationSOPClass, transfersyntax);

	OFCondition cond;
	cond = scu.initNetwork();	
	if(cond.bad())	
		return false;
	
	cond = scu.negotiateAssociation();
	if(cond.bad())
		return false;

	cond = scu.sendECHORequest(0);

	scu.releaseAssociation();

	if(cond == EC_Normal)
	{
		return true;
	}

	return false;
}

void DICOMSender::Cancel()
{
	boost::mutex::scoped_lock lk(mutex);
	cancelEvent = true;
}

void DICOMSender::ClearCancel()
{
	boost::mutex::scoped_lock lk(mutex);
	cancelEvent = false;
}

bool DICOMSender::IsDone()
{
	boost::mutex::scoped_lock lk(mutex);
	return doneEvent;
}

bool DICOMSender::IsCanceled()
{
	boost::mutex::scoped_lock lk(mutex);
	return cancelEvent;
}

void DICOMSender::SetDone(bool state)
{
	boost::mutex::scoped_lock lk(mutex);
	doneEvent = state;
}
