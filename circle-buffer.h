#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/circlebuf.h>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include <windows.h>
#include <util/windows/WinHandle.hpp>

#define CAPTURE_INTERVAL INFINITE
#define NSEC_PER_SEC  1000000000LL

extern enum audio_format get_planar_format(audio_format format);

template<class Data>
class StreamableReader {
	bool _active = false;
	WinHandle _readerThread;
protected:
public:
	StreamableReader()
	{
		_stopStreamingSignal = CreateEvent(nullptr, true, false, nullptr);
	}

	WinHandle _stopStreamingSignal;

	WinHandle stopStreamingSignal()
	{
		return _stopStreamingSignal;
	}

	void Disconnect()
	{
		_active = false;
		SetEvent(_stopStreamingSignal);
		if (_readerThread.Valid())
			WaitForSingleObject(_readerThread, INFINITE);
		ResetEvent(_stopStreamingSignal);
	}
	void Connect(HANDLE h)
	{
		Disconnect();
		_readerThread = h;
		_active = true;
	}

	~StreamableReader()
	{
		Disconnect();
	}
	/*Whatever inheirits this class must implement Read*/
	/*
	void Read(const Data *d)
	{
		...
	}
	*/
};

template<class Data>
class StreamableBuffer {
private:
	std::vector<Data> _Buf;
	std::vector<bool> _Used;
	size_t _writeIndex;
	bool _active = false;

	void incrementWriteIndex()
	{
		_writeIndex = (_writeIndex+1) % _Buf.size();
	}
protected:
public:
	WinHandle _writtenToSignal;
	WinHandle _stopStreamingSignal;

	WinHandle writtenToSignal()
	{
		return _writtenToSignal;
	}

	WinHandle stopStreamingSignal()
	{
		return _stopStreamingSignal;
	}

	StreamableBuffer(size_t count = 32)
	{
		if (count == 0)
			count = 32;
		_Buf.reserve(count);
		_Used.reserve(count);
		for (size_t i = 0; i < count; i++){
			Data c = Data();
			_Buf.push_back(c);
			_Used.push_back(false);
		}
		_writtenToSignal = CreateEvent(nullptr, true, false, nullptr);
		_stopStreamingSignal = CreateEvent(nullptr, true, false, nullptr);
	}

	template<class Callable>
	void Clear(Callable c) {
		for (size_t i = 0; i < _Buf.size(); i++) {
			if (_Used[i])
				std::invoke(c, _Buf[i]);
		}
		std::fill_n(_Used.begin(), _Used.size(), false);
	}

	~StreamableBuffer()
	{
		Disconnect();
	}

	size_t writableIndex()
	{
		return _writeIndex;
	}

	size_t readableIndex()
	{
		return (_writeIndex + 1) % _Buf.size();
	}

	size_t actualIndex(size_t i)
	{
		return i % _Buf.size();
	}

	void Disconnect()
	{
		_active = false;
		SetEvent(_stopStreamingSignal);
	}

	template<class Callable>
	void Write(Data &d, Callable c)
	{
		SetEvent(_writtenToSignal);
		std::invoke(c, d, _Buf[_writeIndex], _Used[_writeIndex]);
		_Used[_writeIndex] = true;
		incrementWriteIndex();
		ResetEvent(_writtenToSignal);
	}

	const Data *Read(size_t i)
	{
		if (_Buf.size() > 0)
			return &_Buf[actualIndex(i)];
		else
			return nullptr;
	}
	
	template<class Reader>
	static DWORD WINAPI Stream(void *data)
	{
		std::string name = "SBData: ";
		std::pair<StreamableBuffer<Data>*, Reader*> *p =
			static_cast<std::pair<StreamableBuffer<Data>*, Reader*>*>(data);

		StreamableBuffer<Data> *device = p->first;
		Reader *source = p->second;

		name += source->Name();
		os_set_thread_name(name.c_str());

		int waitResult = 0;

		size_t readIndex = device->writableIndex();
		HANDLE signals[3] = { device->_writtenToSignal, device->_stopStreamingSignal, source->_stopStreamingSignal };

		while (true) {
			waitResult = WaitForMultipleObjects(3, signals, false, INFINITE);
			switch (waitResult) {
			case WAIT_OBJECT_0:
				while (readIndex != device->writableIndex()) {
					source->Read(device->Read(readIndex));
					readIndex = device->actualIndex(readIndex + 1);
				}
				break;
			case WAIT_OBJECT_0+1:
			case WAIT_OBJECT_0+2:
				delete p;
				return 0;
			case WAIT_ABANDONED_0:
			case WAIT_ABANDONED_0+1:
			case WAIT_ABANDONED_0+2:
			case WAIT_TIMEOUT:
			case WAIT_FAILED:
			default:
				blog(LOG_ERROR, "%i", waitResult);
				delete p;
				return 0;
			}
		}
		delete p;
		return 0;
	}

	template<class Reader>
	void AddListener(Reader &r)
	{
		ResetEvent(_stopStreamingSignal);
		_active = true;
		std::pair<StreamableBuffer<Data>*, Reader*> *pair =
			new std::pair<StreamableBuffer<Data>*, Reader*>(this, &r);
		r.Connect(CreateThread(nullptr, 0, this->Stream<Reader>, pair, 0, nullptr));
	}

	template<class Reader>
	void AddListener(Reader *r)
	{
		ResetEvent(_stopStreamingSignal);
		_active = true;
		std::pair<StreamableBuffer<Data>*, Reader*> *pair =
			new std::pair<StreamableBuffer<Data>*, Reader*>(this, r);
		r->Connect(CreateThread(nullptr, 0, this->Stream<Reader>, pair, 0, nullptr));
	}
};
