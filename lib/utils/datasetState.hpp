#ifndef DATASETSTATE_HPP
#define DATASETSTATE_HPP

#include <cstdint>
#include <exception>
#include <functional>
#include <iosfwd>
#include <map>
#include <set>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"

#include "Config.hpp"
#include "errors.h"
#include "visUtil.hpp"

// This type is used a lot so let's use an alias
using json = nlohmann::json;

// Forward declarations
class datasetState;
class datasetManager;

using state_uptr = std::unique_ptr<datasetState>;

/**
 * @brief A base class for representing state changes done to datasets.
 *
 * This is meant to be subclassed. All subclasses must implement a constructor
 * that calls the base class constructor to set any inner states. As a
 * convention it should pass the data and as a last argument `inner` (which
 * should be optional).
 *
 * @author Richard Shaw, Rick Nitsche
 **/
class datasetState {
public:

    /**
     * @brief Create a datasetState
     *
     * @param inner An internal state that this one wraps. Think of this
     *              like function composition.
     **/
    datasetState(state_uptr inner=nullptr) :
        _inner_state(move(inner)) {};

    virtual ~datasetState() {};

    /**
     * @brief Create a dataset state from a full json serialisation.
     *
     * This will correctly instantiate the correct types and reconstruct all
     * inner states.
     *
     * @param j Full JSON serialisation.
     * @returns The created datasetState or a nullptr in a failure case.
     **/
    static state_uptr from_json(json& j);

    /**
     * @brief Full serialisation of state into JSON.
     *
     * @returns JSON serialisation of state.
     **/
    json to_json() const;

    /**
     * @brief Save the internal data of this instance into JSON.
     *
     * This must be implement by any derived classes and should save the
     * information needed to reconstruct any subclass specific internals.
     * Information of the baseclass (e.g. inner_state) is saved
     * separately.
     *
     * @returns JSON representing the internal state.
     **/
    virtual json data_to_json() const = 0;

    /**
     * @brief Register a derived datasetState type
     *
     * @warning You shouldn't call this directly. It's only public so the macro
     * can call it.
     *
     * @returns Always returns zero.
     **/
    template<typename T>
    static inline int _register_state_type();

    /**
     * @brief Register a base datasetState type
     *
     * If a datasetState type is registered with this, derived sub classes can
     * be found by the datasetManager using the base state.
     *
     * @warning You shouldn't call this directly. It's only public so the macro
     * can call it.
     *
     * @returns Always returns zero.
     **/
    template<typename T>
    static inline int _register_base_state_type();

    /**
     * @brief Compare to another dataset state.
     * @param s    State to compare with.
     * @return True if states identical, False otherwise.
     */
    bool equals(datasetState& s) const;

    /**
     * @brief Get typeids of this state and its inner states.
     * @return A set of state names.
     */
    std::set<std::string> types() const;

private:

    /**
     * @brief Create a datasetState subclass from a json serialisation.
     *
     * @param name  Name of subclass to create.
     * @param data  Serialisation of config.
     * @param inner Inner state to compose with.
     * @returns The created datasetState.
     **/
    static state_uptr _create(std::string name, json & data,
                              state_uptr inner=nullptr);

    // Reference to the internal state
    state_uptr _inner_state = nullptr;

    // List of registered subclass creating functions
    static std::map<string, std::function<state_uptr(json&, state_uptr)>>&
        _registered_types();

    // List of registered functions that check inheritance from base states.
    static std::map<std::string, std::function<bool (const datasetState *)> > &_registered_base_types();

    // Returns all base states of this datasetState.
    std::set<std::string> base_states() const;

    // Add as friend so it can walk the inner state
    friend datasetManager;
};

template<typename T>
inline int datasetState::_register_state_type() {

    // Get the unique name for the type to generate the lookup key. This is
    // the same used by RTTI which is what we use to label the serialised
    // instances.
    std::string key = typeid(T).name();

    DEBUG("Registering state type: %s", key.c_str());

    // Generate a lambda function that creates an instance of the type
    datasetState::_registered_types()[key] =
        [](json & data, state_uptr inner) -> state_uptr {
            return std::make_unique<T>(data, move(inner));
        };
    return 0;
}

template<typename T>
inline int datasetState::_register_base_state_type() {

    // Get the unique name for the type to generate the lookup key. This is
    // the same used by RTTI which is what we use to label the serialised
    // instances.
    std::string key = typeid(T).name();

    DEBUG("Registering base state type: %s", key.c_str());

    // Generate a lambda function that tells if any given state is derived
    // from T.
    datasetState::_registered_base_types()[key] =
        [](const datasetState* t) -> bool {
            return (bool)dynamic_cast<const T*>(t);
        };
    return 0;
}

#define REGISTER_DATASET_STATE(T) int _register_ ## T = \
    datasetState::_register_state_type<T>()
#define REGISTER_BASE_DATASET_STATE(T) int _register_ ## T = \
    datasetState::_register_base_state_type<T>()


// Printing for datasetState
std::ostream& operator<<(std::ostream&, const datasetState&);


/**
 * @brief A dataset state that describes the frequencies in a datatset.
 *
 * @author Richard Shaw, Rick Nitsche
 */
class freqState : public datasetState {
public:
    /**
     * @brief Constructor
     * @param data  The frequency information as serialized by
     *              freqState::to_json().
     * @param inner An inner state or a nullptr.
     */
    freqState(json & data, state_uptr inner) :
        datasetState(move(inner)) {
        try {
            _freqs = data.get<std::vector<std::pair<uint32_t, freq_ctype>>>();
        } catch (std::exception& e) {
             throw std::runtime_error("freqState: Failure parsing json data ("
                                      + data.dump() + "): " + e.what());
        }
    };

    /**
     * @brief Constructor
     * @param freqs The frequency information as a vector of
     *              {frequency ID, frequency index map}.
     * @param inner An inner state (optional).
     */
    freqState(std::vector<std::pair<uint32_t, freq_ctype>> freqs,
              state_uptr inner=nullptr) :
        datasetState(move(inner)),
        _freqs(freqs) {};

    /**
     * @brief Get frequency information (read only).
     *
     * @return The frequency information as a vector of
     *         {frequency ID, frequency index map}
     */
    const std::vector<std::pair<uint32_t, freq_ctype>>& get_freqs() const {
        return _freqs;
    }

private:
    /// Serialize the data of this state in a json object
    json data_to_json() const override {
        json j(_freqs);
        return j;
    }

    /// IDs that describe the subset that this dataset state defines
    std::vector<std::pair<uint32_t, freq_ctype>> _freqs;
};


/**
 * @brief A dataset state that describes the inputs in a datatset.
 *
 * @author Richard Shaw, Rick Nitsche
 */
class inputState : public datasetState {
public:
    /**
     * @brief Constructor
     * @param data  The input information as serialized by
     *              inputState::to_json().
     * @param inner An inner state or a nullptr.
     */
    inputState(json & data, state_uptr inner) :
        datasetState(move(inner)) {
        try {
            _inputs = data.get<std::vector<input_ctype>>();
        } catch (std::exception& e) {
             throw std::runtime_error("inputState: Failure parsing json data ("
                                      + data.dump() + "): " + e.what());
        }
    };

    /**
     * @brief Constructor
     * @param inputs The input information as a vector of
     *               input index maps.
     * @param inner  An inner state (optional).
     */
    inputState(std::vector<input_ctype> inputs, state_uptr inner=nullptr) :
        datasetState(move(inner)),
        _inputs(inputs) {};

    /**
     * @brief Get input information (read only).
     *
     * @return The input information as a vector of input index maps.
     */
    const std::vector<input_ctype>& get_inputs() const {
        return _inputs;
    }

private:
    /// Serialize the data of this state in a json object
    json data_to_json() const override {
        json j(_inputs);
        return j;
    }

    /// The subset that this dataset state defines
    std::vector<input_ctype> _inputs;
};


/**
 * @brief A dataset state that describes the products in a datatset.
 *
 * @author Richard Shaw, Rick Nitsche
 */
class prodState : public datasetState {
public:
    /**
     * @brief Constructor
     * @param data  The product information as serialized by
     *              prodState::to_json().
     * @param inner An inner state or a nullptr.
     */
    prodState(json & data, state_uptr inner) :
        datasetState(move(inner)) {
        try {
            _prods = data.get<std::vector<prod_ctype>>();
        } catch (std::exception& e) {
             throw std::runtime_error("prodState: Failure parsing json data ("
                                      + data.dump() + "): " + e.what());
        }
    };

    /**
     * @brief Constructor
     * @param prods The product information as a vector of
     *              product index maps.
     * @param inner An inner state (optional).
     */
    prodState(std::vector<prod_ctype> prods, state_uptr inner=nullptr) :
        datasetState(move(inner)),
        _prods(prods) {};

    /**
     * @brief Get product information (read only).
     *
     * @return The prod information as a vector of product index maps.
     */
    const std::vector<prod_ctype>& get_prods() const {
        return _prods;
    }

private:
    /// Serialize the data of this state in a json object
    json data_to_json() const override {
        json j(_prods);
        return j;
    }

    /// IDs that describe the subset that this dataset state defines
    std::vector<prod_ctype> _prods;
};


/**
 * @brief A dataset state that keeps the time information of a datatset.
 *
 * @author Rick Nitsche
 */
class timeState : public datasetState {
public:
    /**
     * @brief Constructor
     * @param data  The time information as serialized by
     *              timeState::to_json().
     * @param inner An inner state or a nullptr.
     */
    timeState(json & data, state_uptr inner) :
        datasetState(move(inner)) {
        try {
            _times = data.get<std::vector<time_ctype>>();
        } catch (std::exception& e) {
             throw std::runtime_error("timeState: Failure parsing json data ("
                                      + data.dump() + "): " + e.what());
        }
    };

    /**
     * @brief Constructor
     * @param times The time information as a vector of
     *              time index maps.
     * @param inner An inner state (optional).
     */
    timeState(std::vector<time_ctype> times, state_uptr inner=nullptr) :
        datasetState(move(inner)),
        _times(times) {};

    /**
     * @brief Get time information (read only).
     *
     * @return The time information as a vector of time index maps.
     */
    const std::vector<time_ctype>& get_times() const {
        return _times;
    }

private:
    /// Serialize the data of this state in a json object
    json data_to_json() const override {
        json j(_times);
        return j;
    }

    /// Time index map of the dataset state.
    std::vector<time_ctype> _times;
};

/**
 * @brief A dataset state that keeps the eigenvalues of a datatset.
 *
 * @author Rick Nitsche
 */
class eigenvalueState : public datasetState {
public:
    /**
     * @brief Constructor
     * @param data  The eigenvalues as serialized by
     *              eigenvalueState::to_json().
     * @param inner An inner state or a nullptr.
     */
    eigenvalueState(json & data, state_uptr inner) :
        datasetState(move(inner)) {
        try {
            _ev = data.get<std::vector<uint32_t>>();
        } catch (std::exception& e) {
             throw std::runtime_error("eigenvectorState: Failure parsing json "\
                                      "data (" + data.dump() + "): "
                                      + e.what());
        }
    };

    /**
     * @brief Constructor
     * @param ev The eigenvalues.
     * @param inner An inner state (optional).
     */
    eigenvalueState(std::vector<uint32_t> ev, state_uptr inner=nullptr) :
        datasetState(move(inner)),
        _ev(ev) {};

    /**
     * @brief Get eigenvalues (read only).
     *
     * @return The eigenvalues.
     */
    const std::vector<uint32_t>& get_ev() const {
        return _ev;
    }

private:
    /// Serialize the data of this state in a json object
    json data_to_json() const override {
        json j(_ev);
        return j;
    }

    /// Eigenvalues of the dataset state.
    std::vector<uint32_t> _ev;
};


std::vector<stack_ctype> invert_stack(
    uint32_t num_stack, const std::vector<rstack_ctype>& stack_map);


/**
 * @brief A dataset state that describes a redundant baseline stacking.
 *
 * @author Richard Shaw
 */
class stackState : public datasetState {
public:
    /**
     * @brief Constructor
     * @param data  The stack information as serialized by
     *              stackState::to_json().
     * @param inner An inner state or a nullptr.
     */
    stackState(json& data, state_uptr inner) :
        datasetState(move(inner))
    {
        try {
            _rstack_map = data["rstack"].get<std::vector<rstack_ctype>>();
            _num_stack = data["num_stack"].get<uint32_t>();
        } catch (std::exception& e) {
             throw std::runtime_error("stackState: Failure parsing json data: "
                                      + std::string(e.what()));
        }
    };

    /**
     * @brief Constructor
     * @param rstack_map Definition of how the products were stacked.
     * @param num_stack Number of stacked visibilites.
     * @param inner  An inner state (optional).
     */
    stackState(uint32_t num_stack, std::vector<rstack_ctype>&& rstack_map,
               state_uptr inner=nullptr) :
        datasetState(std::move(inner)),
        _num_stack(num_stack),
        _rstack_map(rstack_map) {}

    /**
     * @brief Get stack map information (read only).
     *
     * For every product this says which stack to add the product into and
     * whether it needs conjugating before doing so.
     *
     * @return The stack map.
     */
    const std::vector<rstack_ctype>& get_rstack_map() const {
        return _rstack_map;
    }

    /**
     * @brief Get the number of stacks.
     *
     * @return The number of stacks.
     */
    uint32_t get_num_stack() const {
        return _num_stack;
    }

    /**
     * @brief Calculate and return the stack->prod mapping.
     *
     * This is calculated on demand and so a full fledged vector is returned.
     *
     * @returns The stack map.
     **/
    std::vector<stack_ctype> get_stack_map() const
    {
        return invert_stack(_num_stack, _rstack_map);
    }

    /// Serialize the data of this state in a json object
    json data_to_json() const override
    {
        return {{"rstack", _rstack_map }, {"num_stack", _num_stack}};
    }

private:

    /// Total number of stacks
    uint32_t _num_stack;

    /// The stack definition
    std::vector<rstack_ctype> _rstack_map;
};


/**
 * @brief A dataset state that describes all the metadata that is written to
 * file as "attributes", but not defined by other states yet.
 *
 * @author Rick Nitsche
 */
class metadataState : public datasetState {
public:
    /**
     * @brief Constructor
     * @param data  The metadata as serialized by
     *              metadataState::to_json():
     * weight_type: string
     * instrument_name: string
     * git_version_number: string
     *
     * @param inner An inner state or a nullptr.
     */
    metadataState(json & data, state_uptr inner) :
        datasetState(move(inner)) {
        try {
            _weight_type = data.at("weight_type").get<std::string>();
            _instrument_name = data.at("instrument_name").get<std::string>();
            _git_version_tag = data.at("git_version_tag").get<std::string>();
        } catch (std::exception& e) {
             throw std::runtime_error("metadataState: Failure parsing json " \
                                      "data (" + data.dump() + "): "
                                      + e.what());
        }
    }

    /**
     * @brief Constructor
     * @param weight_type       The weight type attribute.
     * @param instrument_name   The instrument name attribute.
     * @param git_version_tag   The git version tag attribute.
     * @param inner             An inner state (optional).
     */
    metadataState(std::string weight_type, std::string instrument_name,
                  std::string git_version_tag, state_uptr inner=nullptr) :
        datasetState(move(inner)), _weight_type(weight_type),
        _instrument_name(instrument_name), _git_version_tag(git_version_tag) {}

    /**
     * @brief Get the weight type (read only).
     *
     * @return The weigh type.
     */
    const std::string& get_weight_type() const {
        return _weight_type;
    }

    /**
     * @brief Get the instrument name (read only).
     *
     * @return The instrument name.
     */
    const std::string& get_instrument_name() const {
        INFO("instrument name: %s", _instrument_name.c_str());
        return _instrument_name;
    }

    /**
     * @brief Get the git version tag (read only).
     *
     * @return The git version tag.
     */
    const std::string& get_git_version_tag() const {
        return _git_version_tag;
    }

private:
    /// Serialize the data of this state in a json object
    json data_to_json() const override {
        json j;
        j["weight_type"] = _weight_type;
        j["instrument_name"] = _instrument_name;
        j["git_version_tag"] = _git_version_tag;
        return j;
    }

    // the actual metadata
    std::string _weight_type, _instrument_name, _git_version_tag;
};

/**
 * @brief A base state for all types of gating.
 *
 * TODO: make sure the base state can't be instanciated?
 *
 * @author Rick Nitsche
 */
class gatingState : public datasetState {
public:
    /**
     * @brief Constructor
     * @param inner             An inner state (optional).
     */
    gatingState(state_uptr inner=nullptr) :
        datasetState(move(inner)) {}
};

/**
 * @brief A state to describe pulsar gating.
 *
 * @author Richard Shaw, Rick Nitsche
 **/
class pulsarGatingState : public gatingState {
public:
    /**
     * @brief Construct a pulsar gating state
     *
     * @param data  The metadata as serialized by to_json():
     *              name: string
     * @param inner Inner state.
     **/
    pulsarGatingState(json & data, state_uptr inner) :
        gatingState(move(inner)) {
        _name = data.at("name").get<std::string>();
    }

    /**
     * @brief Construct a pulsar gating state
     *
     * @param  name   The name of the pulsar.
     * @param  inner  Inner state.
     **/
    pulsarGatingState(const std::string name, state_uptr inner=nullptr) :
        gatingState(move(inner)), _name(name) {}

    /**
     * @brief Get the name of the pulsar.
     * @return The name of the pulsar.
     */
    const std::string& get_name() const {
        return _name;
    }

private:
    /// Serialize the data of this state in a json object
    json data_to_json() const override {
        json j;
        j["name"] = _name;
        return j;
    }

    std::string _name;
};

/**
 * @brief A state to describe noise source gating.
 *
 * At the moment just for proof of concept. Complete this states data or remove it.
 *
 * @author Richard Shaw, Rick Nitsche
 **/
class noiseSourceGatingState : public gatingState {
public:
    /**
     * @brief Construct a noise source gating state
     *
     * @param data  The metadata as serialized by to_json():
     *              name: string
     * @param inner Inner state.
     **/
    noiseSourceGatingState(json & data, state_uptr inner) :
        gatingState(move(inner)) {
        _foo = data.at("foo").get<std::string>();
    }

    /**
     * @brief Construct a noise source gating state
     *
     * @param  foo    TODO: replace.
     * @param  inner  Inner state.
     **/
    noiseSourceGatingState(const std::string foo, state_uptr inner=nullptr) :
        gatingState(move(inner)), _foo(foo) {}

    /**
     * @brief Get foo.
     * @return The foo.
     */
    const std::string& get_foo() const {
        return _foo;
    }

private:
    /// Serialize the data of this state in a json object
    json data_to_json() const override {
        json j;
        j["foo"] = _foo;
        return j;
    }

    std::string _foo;
};

#endif // DATASETSTATE_HPP
